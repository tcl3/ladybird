/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <LibCore/EventLoop.h>
#include <LibGC/Root.h>
#include <LibHTTP/HeaderList.h>
#include <LibRequests/Request.h>
#include <LibWeb/Fetch/Engine/FetchBodyDeliveryGate.h>
#include <LibWeb/Fetch/Fetching/ClientContextSnapshots.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Fetching/NetworkBodyPump.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/RemoteEnvironmentSettings.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/ServiceWorker/FetchChannel.h>

namespace Web::ServiceWorker {

static void forget_fetch_event(FetchEventId);

namespace {

// The body of a response that a service worker gave a fetch event, which the worker's process delivers in chunks as
// far as the body's consumer allows.
class FetchEventBodyTransport final : public Fetch::Engine::FetchBodyDeliveryGate::Transport {
public:
    FetchEventBodyTransport(WeakPtr<FetchChannel> channel, FetchEventId fetch_id)
        : m_channel(move(channel))
        , m_fetch_id(fetch_id)
    {
    }

    void did_deliver(u64 byte_count) { m_delivered_byte_count += byte_count; }

    virtual void resume_body_delivery_up_to(u64 byte_count) override
    {
        // NB: The worker's process may deliver this many bytes past those delivered so far, in all.
        if (auto channel = m_channel.strong_ref(); channel && channel->is_open())
            channel->async_allow_fetch_response_body(m_fetch_id, m_delivered_byte_count + byte_count);
    }

    virtual void pause_body_delivery() override
    {
        // NB: The worker's process delivers only what it was allowed to, so delivery pauses once it has.
    }

    virtual void stop() override
    {
        if (auto channel = m_channel.strong_ref(); channel && channel->is_open())
            channel->async_cancel_fetch(m_fetch_id);

        // NB: The worker's process sends nothing more of a cancelled body. The gate that stops this transport is let go
        //     once it has.
        Core::deferred_invoke([fetch_id = m_fetch_id] { forget_fetch_event(fetch_id); });
    }

    virtual void misbehaved(StringView reason) override
    {
        dbgln("A service worker's process misbehaved delivering a response body: {}", reason);
        if (auto channel = m_channel.strong_ref())
            channel->shutdown();
    }

private:
    WeakPtr<FetchChannel> m_channel;
    FetchEventId m_fetch_id { 0 };
    u64 m_delivered_byte_count { 0 };
};

// A fetch event that waits for its result, and then for the rest of its response's body.
struct PendingFetchEvent {
    GC::Root<GC::Function<void(RefPtr<Fetch::Infrastructure::Response>)>> on_complete;
    HTML::EnvironmentId client;
    ServiceWorkerId service_worker { 0 };
    bool has_settled { false };

    RefPtr<Fetch::Engine::FetchByteChannel> body_channel;
    RefPtr<Fetch::Engine::FetchBodyDeliveryGate> delivery_gate;
    GC::Root<Fetch::Fetching::NetworkBodyPump> body_pump;
    RefPtr<FetchEventBodyTransport> body_transport;
};

// The channel of a client to its active service worker's process, or the fetch events that wait for the UI process to
// set it up.
struct ChannelEntry {
    HTML::EnvironmentId client;
    ServiceWorkerId service_worker { 0 };
    RefPtr<FetchChannel> channel;
    Optional<ServiceWorkerRequestId> channel_request;
    Vector<FetchEventId> fetch_events_waiting_for_channel;
};

struct FetchChannelState {
    HashMap<FetchEventId, PendingFetchEvent> pending_fetch_events;
    HashMap<FetchEventId, FetchEventRequest> requests_waiting_for_channel;
    Vector<ChannelEntry> channels;
    FetchEventId next_fetch_id { 0 };
    ServiceWorkerRequestId next_channel_request_id { 0 };
};

}

static FetchChannelState& fetch_channel_state()
{
    // NB: The fetch events in here are those of the agent whose fetches wait for them.
    static thread_local NeverDestroyed<FetchChannelState> state;
    return *state;
}

void forget_fetch_channels_of_this_agent()
{
    auto& state = fetch_channel_state();
    // NB: The steps of a pending fetch event are cells of this agent. Its channels have no more fetches to answer.
    state.pending_fetch_events.clear();
    state.requests_waiting_for_channel.clear();
    auto channels = move(state.channels);
    for (auto& entry : channels) {
        if (entry.channel)
            entry.channel->shutdown();
    }
}

void forget_fetch_event(FetchEventId fetch_id)
{
    fetch_channel_state().pending_fetch_events.remove(fetch_id);
}

// https://w3c.github.io/ServiceWorker/#create-fetch-event-and-dispatch
// 22.2. Return networkError.
static void fail_fetch_event(FetchEventId fetch_id)
{
    auto pending = fetch_channel_state().pending_fetch_events.take(fetch_id);
    fetch_channel_state().requests_waiting_for_channel.remove(fetch_id);
    if (!pending.has_value())
        return;

    // NB: A fetch event whose result arrived loses the rest of its response's body.
    if (pending->has_settled) {
        if (pending->body_pump)
            pending->body_pump->handle_network_error("The service worker's process went away"sv);
        if (pending->delivery_gate)
            pending->delivery_gate->detach_transport();
        return;
    }
    pending->on_complete->function()(Fetch::Infrastructure::Response::network_error("The service worker could not handle the fetch event"_string));
}

static void fail_fetch_events_of(HTML::EnvironmentId const& client, ServiceWorkerId service_worker)
{
    Vector<FetchEventId> fetch_ids;
    for (auto const& [fetch_id, pending] : fetch_channel_state().pending_fetch_events) {
        if (pending.client == client && pending.service_worker == service_worker)
            fetch_ids.append(fetch_id);
    }
    for (auto fetch_id : fetch_ids)
        fail_fetch_event(fetch_id);
}

FetchChannel::FetchChannel(NonnullOwnPtr<IPC::Transport> transport, HTML::EnvironmentId client, ServiceWorkerId service_worker)
    : IPC::ConnectionToServer<ServiceWorkerFetchClientEndpoint, ServiceWorkerFetchServerEndpoint>(*this, move(transport))
    , m_client(move(client))
    , m_service_worker(service_worker)
{
}

FetchChannel::~FetchChannel() = default;

// https://w3c.github.io/ServiceWorker/#create-fetch-event-and-dispatch
// 17.4. If task is discarded, set handleFetchFailed to true.
// NB: The worker's process discards the tasks of its fetch events when the worker terminates, which closes the channel.
void FetchChannel::die()
{
    // NB: The channel is let go once it has closed.
    Core::deferred_invoke([protected_this = NonnullRefPtr { *this }] { });

    auto client = m_client;
    auto service_worker = m_service_worker;
    fetch_channel_state().channels.remove_all_matching([&](ChannelEntry const& entry) {
        return entry.channel.ptr() == this;
    });
    fail_fetch_events_of(client, service_worker);
}

// NB: A channel whose client is gone from this process, and whose fetch events have all ended, is closed.
static void close_channels_of_departed_clients()
{
    auto& state = fetch_channel_state();
    auto has_fetch_events = [&](ChannelEntry const& entry) {
        return any_of(state.pending_fetch_events, [&](auto const& pending) {
            return pending.value.client == entry.client && pending.value.service_worker == entry.service_worker;
        });
    };
    Vector<NonnullRefPtr<FetchChannel>> channels_to_close;
    state.channels.remove_all_matching([&](ChannelEntry const& entry) {
        if (!entry.channel || HTML::environment_settings_object_with_id(entry.client) || has_fetch_events(entry))
            return false;
        channels_to_close.append(*entry.channel);
        return true;
    });
    for (auto& channel : channels_to_close)
        channel->shutdown();
}

// https://w3c.github.io/ServiceWorker/#fetch-event-respondwith
// 10.2.4. Let potentialResponse be a copy of response's associated response, except for its body.
static NonnullRefPtr<Fetch::Infrastructure::Response> internal_response_from(FetchEventResponse const& descriptor)
{
    auto internal_response = Fetch::Infrastructure::Response::create();
    internal_response->set_status(descriptor.status);
    internal_response->set_status_message(descriptor.status_message);
    internal_response->set_header_list(HTTP::HeaderList::create(descriptor.header_list));
    internal_response->set_url_list(descriptor.url_list);
    internal_response->set_cors_exposed_header_name_list(descriptor.cors_exposed_header_name_list);
    internal_response->set_range_requested(descriptor.range_requested);
    internal_response->set_request_includes_credentials(descriptor.request_includes_credentials);
    internal_response->set_timing_allow_passed(descriptor.timing_allow_passed);
    internal_response->set_redirect_taint(descriptor.redirect_taint);
    return internal_response;
}

static NonnullRefPtr<Fetch::Infrastructure::Response> response_of_type(Fetch::Infrastructure::Response::Type type, NonnullRefPtr<Fetch::Infrastructure::Response> internal_response)
{
    using Type = Fetch::Infrastructure::Response::Type;
    switch (type) {
    case Type::Basic:
        return Fetch::Infrastructure::BasicFilteredResponse::create(move(internal_response));
    case Type::CORS:
        return Fetch::Infrastructure::CORSFilteredResponse::create(move(internal_response));
    case Type::Opaque:
        return Fetch::Infrastructure::OpaqueFilteredResponse::create(move(internal_response));
    case Type::OpaqueRedirect:
        return Fetch::Infrastructure::OpaqueRedirectFilteredResponse::create(move(internal_response));
    case Type::Default:
        return internal_response;
    case Type::Error:
        break;
    }
    VERIFY_NOT_REACHED();
}

// https://w3c.github.io/ServiceWorker/#create-fetch-event-and-dispatch
void FetchChannel::fetch_event_settled(u64 fetch_id, FetchEventOutcome outcome, Optional<FetchEventResponse> descriptor)
{
    auto pending = fetch_channel_state().pending_fetch_events.get(fetch_id);
    if (!pending.has_value() || pending->has_settled || pending->client != m_client || pending->service_worker != m_service_worker) {
        dbgln("A service worker's process settled a fetch event it was not given");
        shutdown();
        return;
    }

    switch (outcome) {
    // 21.4. Return null.
    case FetchEventOutcome::Fallback: {
        auto on_complete = move(pending->on_complete);
        fetch_channel_state().pending_fetch_events.remove(fetch_id);
        on_complete->function()(nullptr);
        return;
    }
    // 21.1.2. Return networkError.
    // 22.2. Return networkError.
    case FetchEventOutcome::NetworkError: {
        auto on_complete = move(pending->on_complete);
        fetch_channel_state().pending_fetch_events.remove(fetch_id);
        on_complete->function()(Fetch::Infrastructure::Response::network_error("The service worker responded with a network error"_string));
        return;
    }
    // 24. Return response.
    case FetchEventOutcome::Response:
        break;
    }

    if (!descriptor.has_value()) {
        dbgln("A service worker's process settled a fetch event with a response it did not describe");
        shutdown();
        return;
    }

    // NB: A response of the "error" type is a network error.
    if (descriptor->type == Fetch::Infrastructure::Response::Type::Error) {
        auto on_complete = move(pending->on_complete);
        fetch_channel_state().pending_fetch_events.remove(fetch_id);
        on_complete->function()(Fetch::Infrastructure::Response::network_error("The service worker responded with a network error"_string));
        return;
    }
    auto internal_response = internal_response_from(*descriptor);

    // NB: A response without a body is all there is of the fetch event.
    if (!descriptor->has_body) {
        auto on_complete = move(pending->on_complete);
        fetch_channel_state().pending_fetch_events.remove(fetch_id);
        on_complete->function()(response_of_type(descriptor->type, move(internal_response)));
        return;
    }

    // https://w3c.github.io/ServiceWorker/#fetch-event-respondwith
    // 10.2.5.7. Set potentialResponse's body to a new body whose stream is newStream.
    // NB: The worker's process reads the response's body and delivers its bytes here, as a network body's are delivered.
    auto body = Fetch::Fetching::create_delivered_response_body();
    body.body_pump->set_response(internal_response);
    internal_response->set_body(Fetch::Infrastructure::ResponseBody { body.channel, {} });

    auto transport = adopt_ref(*new FetchEventBodyTransport(make_weak_ptr<FetchChannel>(), fetch_id));
    pending->has_settled = true;
    pending->body_channel = body.channel;
    pending->delivery_gate = body.delivery_gate;
    pending->body_pump = GC::make_root(body.body_pump);
    pending->body_transport = transport;
    auto on_complete = pending->on_complete;
    body.delivery_gate->attach_transport(transport);
    on_complete->function()(response_of_type(descriptor->type, move(internal_response)));
}

void FetchChannel::fetch_response_body_data(u64 fetch_id, ByteBuffer data)
{
    auto pending = fetch_channel_state().pending_fetch_events.get(fetch_id);
    if (!pending.has_value() || !pending->has_settled || !pending->body_pump || pending->client != m_client)
        return;
    pending->body_transport->did_deliver(data.size());
    pending->body_pump->handle_network_data(Requests::ResponseData::from_bytes(data));
}

void FetchChannel::fetch_response_body_end(u64 fetch_id, bool succeeded)
{
    auto pending = fetch_channel_state().pending_fetch_events.get(fetch_id);
    if (!pending.has_value() || !pending->has_settled || !pending->body_pump || pending->client != m_client)
        return;
    auto ended = fetch_channel_state().pending_fetch_events.take(fetch_id).release_value();
    ended.delivery_gate->detach_transport();
    if (succeeded)
        ended.body_pump->handle_network_complete({});
    else
        ended.body_pump->handle_network_error("The service worker's response body errored"sv);
}

// https://w3c.github.io/ServiceWorker/#create-fetch-event-and-dispatch
// 17.4.3. Let requestObject be the result of creating a Request object, given request, a new Headers object's guard
//         which is "immutable", abortController's signal, and workerRealm.
// NB: The worker's process creates requestObject from what this says of request.
static FetchEventRequest describe_request(Fetch::Infrastructure::Request const& request)
{
    Optional<ByteBuffer> body;
    request.body().visit(
        [](Empty) { },
        [&](ByteBuffer const& bytes) { body = MUST(ByteBuffer::copy(bytes)); },
        [&](Fetch::Infrastructure::FetchBody const& fetch_body) {
            if (auto const& source = fetch_body.source(); source.has_value())
                body = MUST(source->copy_to_byte_buffer());
            else
                body = ByteBuffer {};
        });

    // 17.4.8. If client is not null, initialize e's clientId attribute to client's id.
    // AD-HOC: A navigation request's fetch event has an empty clientId, as in other engines, even when the navigation has
    //         an initiating client.
    Optional<String> client_id;
    if (request.is_navigation_request())
        client_id = {};
    else if (auto client = Fetch::Fetching::resolve_client(request))
        client_id = client->id.to_string();
    else if (auto remote_client = Fetch::Fetching::remote_client(request))
        client_id = remote_client->settings.id.to_string();

    return FetchEventRequest {
        .method = request.method(),
        // AD-HOC: The fetch event of a request that a redirect brought to the service worker is for the URL the request
        //         was redirected to, as in other engines.
        .url_list = { request.current_url() },
        .header_list = request.header_list()->headers(),
        .body = move(body),
        .destination = request.destination(),
        .mode = request.mode(),
        .credentials_mode = request.credentials_mode(),
        .cache_mode = request.cache_mode(),
        .redirect_mode = request.redirect_mode(),
        .referrer = request.referrer(),
        .referrer_policy = request.referrer_policy(),
        .integrity_metadata = request.integrity_metadata().to_utf8_but_should_be_ported_to_utf16(),
        .keepalive = request.keepalive(),
        .reload_navigation = request.reload_navigation(),
        .history_navigation = request.history_navigation(),
        .client_id = move(client_id),
        .replaces_client_id = request.replaces_client_id().to_string(),
        .navigation_preload_header_value = {},
    };
}

void dispatch_fetch_event(Page& page, HTML::EnvironmentId const& client, ServiceWorkerDescriptor const& active_worker, Fetch::Infrastructure::Request const& request, Optional<ByteString> navigation_preload_header_value, HandleFetchCallback on_complete)
{
    close_channels_of_departed_clients();

    auto& state = fetch_channel_state();
    auto fetch_id = ++state.next_fetch_id;
    state.pending_fetch_events.set(fetch_id, PendingFetchEvent {
                                                 .on_complete = GC::make_root(on_complete),
                                                 .client = client,
                                                 .service_worker = active_worker.id,
                                                 .has_settled = false,
                                                 .body_channel = {},
                                                 .delivery_gate = {},
                                                 .body_pump = {},
                                                 .body_transport = {},
                                             });
    auto request_descriptor = describe_request(request);
    request_descriptor.navigation_preload_header_value = move(navigation_preload_header_value);

    auto entry = state.channels.find_if([&](ChannelEntry const& entry) {
        return entry.client == client && entry.service_worker == active_worker.id;
    });
    if (entry != state.channels.end() && entry->channel) {
        entry->channel->async_dispatch_fetch_event(fetch_id, request_descriptor);
        return;
    }

    state.requests_waiting_for_channel.set(fetch_id, move(request_descriptor));
    if (entry != state.channels.end()) {
        entry->fetch_events_waiting_for_channel.append(fetch_id);
        return;
    }

    // NB: The UI process runs the worker, as Run Service Worker does, and sets up a channel to the worker's process.
    auto channel_request = ++state.next_channel_request_id;
    state.channels.append(ChannelEntry {
        .client = client,
        .service_worker = active_worker.id,
        .channel = {},
        .channel_request = channel_request,
        .fetch_events_waiting_for_channel = { fetch_id },
    });
    if (!page.client().page_did_request_service_worker_fetch_channel(channel_request, client, active_worker.id))
        service_worker_fetch_channel_ready(channel_request, {});
}

void service_worker_fetch_channel_ready(ServiceWorkerRequestId request_id, Optional<IPC::TransportHandle> handle)
{
    auto& state = fetch_channel_state();
    auto entry = state.channels.find_if([&](ChannelEntry const& entry) { return entry.channel_request == request_id; });
    if (entry == state.channels.end())
        return;

    // https://w3c.github.io/ServiceWorker/#create-fetch-event-and-dispatch
    // 16. If the result of running the Run Service Worker algorithm with activeWorker is failure, then set
    //     handleFetchFailed to true.
    // NB: The UI process gives no channel for a worker that could not run.
    Optional<NonnullOwnPtr<IPC::Transport>> transport;
    if (handle.has_value()) {
        if (auto transport_or_error = handle->create_transport(); !transport_or_error.is_error())
            transport = transport_or_error.release_value();
    }
    if (!transport.has_value()) {
        auto client = entry->client;
        auto service_worker = entry->service_worker;
        state.channels.remove(entry.index());
        fail_fetch_events_of(client, service_worker);
        return;
    }

    auto channel = adopt_ref(*new FetchChannel(transport.release_value(), entry->client, entry->service_worker));
    entry->channel = channel;
    entry->channel_request = {};
    for (auto fetch_id : exchange(entry->fetch_events_waiting_for_channel, {})) {
        if (auto request = state.requests_waiting_for_channel.take(fetch_id); request.has_value())
            channel->async_dispatch_fetch_event(fetch_id, request.release_value());
    }
}

}
