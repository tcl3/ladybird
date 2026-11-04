/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibIPC/TransportHandle.h>
#include <LibURL/Site.h>
#include <LibWebCommon/SecureContexts/AbstractOperations.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalEnvironmentSettingsObject.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/ServiceWorkerRegistry.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebWorkerClient.h>
#include <LibWebView/WorkerAgentManager.h>
#include <signal.h>

namespace WebView {

using namespace Web::ServiceWorker;

static constexpr auto new_service_worker_start_deadline = AK::Duration::from_seconds(5 * 60);
static constexpr auto lifecycle_event_deadline_grace_period = AK::Duration::from_seconds(10);

static constexpr auto installed_service_worker_start_deadline = AK::Duration::from_seconds(60);

static bool is_test_mode()
{
    return Application::web_content_options().is_test_mode == IsTestMode::Yes;
}

static AK::Duration lifecycle_event_deadline()
{
    // NB: Tests of what happens at the deadline would otherwise take minutes.
    if (is_test_mode())
        return AK::Duration::from_seconds(3);
    return AK::Duration::from_seconds(5 * 60);
}

// AD-HOC: The running workers of a partition stop this long after its last client goes away, as WebKit's
//         defaultTerminationDelay has them do. Tests shorten the delay, as WebKit's tests do.
static AK::Duration termination_delay()
{
    if (is_test_mode())
        return AK::Duration::from_milliseconds(100);
    return AK::Duration::from_seconds(10);
}

// NB: A worker that starts, installs, activates or handles an event answers a heartbeat before the next one, or stops.
static AK::Duration heartbeat_interval()
{
    if (is_test_mode())
        return AK::Duration::from_seconds(1);
    return AK::Duration::from_seconds(60);
}

static Vector<ServiceWorkerRegistry*>& live_registries()
{
    static auto& registries = *new Vector<ServiceWorkerRegistry*>;
    return registries;
}

ServiceWorkerClientConnection::ServiceWorkerClientConnection(WebContentClient& client)
    : m_connection(WeakPtr<WebContentClient> { client })
{
}

ServiceWorkerClientConnection::ServiceWorkerClientConnection(WebWorkerClient& client)
    : m_connection(WeakPtr<WebWorkerClient> { client })
{
}

void const* ServiceWorkerClientConnection::identity() const
{
    return m_connection.visit([](auto const& connection) -> void const* { return connection.ptr(); });
}

void ServiceWorkerClientConnection::service_worker_job_settled(ServiceWorkerRequestId request_id, ServiceWorkerJobResult const& result) const
{
    m_connection.visit([&](auto const& connection) {
        if (connection && connection->is_open())
            connection->async_service_worker_job_settled(request_id, result);
    });
}

void ServiceWorkerClientConnection::service_worker_registration_lookup_finished(ServiceWorkerRequestId request_id, Vector<ServiceWorkerRegistrationSnapshot> const& registrations) const
{
    m_connection.visit([&](auto const& connection) {
        if (connection && connection->is_open())
            connection->async_service_worker_registration_lookup_finished(request_id, registrations);
    });
}

unsigned ServiceWorkerRegistry::RegistrationKeyTraits::hash(RegistrationKey const& key)
{
    auto hash = pair_int_hash(Traits<Web::StorageAPI::StorageKey>::hash(key.partition.storage_key), key.serialized_scope_url.hash());
    if (key.partition.top_level_site.has_value())
        hash = pair_int_hash(hash, key.partition.top_level_site->hash());
    return pair_int_hash(hash, static_cast<unsigned>(key.partition.has_cross_site_ancestor));
}

ServiceWorkerRegistry::ServiceWorkerRegistry(IsPrivate is_private)
    : m_is_private(is_private)
{
    live_registries().append(this);
}

ServiceWorkerRegistry::~ServiceWorkerRegistry()
{
    live_registries().remove_first_matching([this](auto* registry) { return registry == this; });

    for (auto& [id, worker] : m_service_workers) {
        if (worker->agent_id.has_value())
            WorkerAgentManager::the().close_service_worker_agent(*worker->agent_id);
    }
}

void ServiceWorkerRegistry::environment_was_discarded(Web::HTML::EnvironmentId const& environment_id)
{
    for (auto* registry : live_registries()) {
        registry->client_did_dispatch_dom_content_loaded(environment_id);
        registry->m_ready_requests.remove_all_matching([&](ReadyRequest const& ready_request) {
            return ready_request.environment_id == environment_id;
        });

        // https://w3c.github.io/ServiceWorker/#on-client-unload-algorithm
        // The user agent must run these steps as part of when a service worker client unloads via unloading document
        // cleanup steps or termination.
        // NB: The canonical record of a client goes away once its document unloads or its worker terminates.
        registry->handle_service_worker_client_unload(environment_id);

        // NB: The environment's records are only now going away, so the clients that remain are counted after.
        Core::deferred_invoke([weak_registry = registry->make_weak_ptr()] {
            if (auto* registry = weak_registry.ptr())
                registry->stop_workers_of_partitions_without_clients();
        });
    }
}

// https://w3c.github.io/ServiceWorker/#on-client-unload-algorithm
void ServiceWorkerRegistry::handle_service_worker_client_unload(Web::HTML::EnvironmentId const& client)
{
    // 1. Run the following steps atomically.
    // 2. Let registration be the service worker registration used by client.
    auto index = m_controlled_clients.find_first_index_if([&](ControlledClient const& controlled_client) { return controlled_client.id == client; });

    // 3. If registration is null, abort these steps.
    if (!index.has_value())
        return;
    auto unloaded_client = m_controlled_clients.take(*index);
    auto worker = m_service_workers.get(unloaded_client.active_service_worker);
    auto* registration = worker.has_value() ? registration_by_id((*worker)->registration_id) : nullptr;
    forget_service_worker_if_unused(unloaded_client.active_service_worker);
    if (!registration)
        return;

    // 4. If any other service worker client is using registration, abort these steps.
    if (any_service_worker_client_is_using(*registration))
        return;

    // 5. If registration is unregistered, invoke Try Clear Registration with registration.
    if (is_unregistered(*registration)) {
        auto registration_id = registration->id;
        try_clear_registration(*registration);

        // NB: Nothing refers to a registration that Try Clear Registration cleared.
        registration = registration_by_id(registration_id);
        if (!registration)
            return;
    }

    // 6. Invoke Try Activate with registration.
    try_activate(*registration);
}

// https://w3c.github.io/ServiceWorker/#dfn-use
bool ServiceWorkerRegistry::is_using(ControlledClient const& client, Registration const& registration) const
{
    // When a service worker client is controlled by a service worker, it is said that the service worker client is using
    // the service worker's containing service worker registration.
    auto worker = m_service_workers.get(client.active_service_worker);
    return worker.has_value() && (*worker)->registration_id == registration.id;
}

bool ServiceWorkerRegistry::any_service_worker_client_is_using(Registration const& registration) const
{
    return any_of(m_controlled_clients, [&](ControlledClient const& client) { return is_using(client, registration); });
}

void ServiceWorkerRegistry::for_each_hosted_client(Function<IterationDecision(HostedClient const&)> const& callback) const
{
    auto decision = IterationDecision::Continue;
    WebContentClient::for_each_client([&](WebContentClient& client) {
        if (client.is_private() != m_is_private)
            return IterationDecision::Continue;
        client.for_each_page([&](WebContentPage& page) {
            page.for_each_hosted_environment([&](CanonicalEnvironmentSettingsObject const& settings, bool has_cross_site_ancestor) {
                decision = callback(HostedClient {
                    .connection = ServiceWorkerClientConnection { client },
                    .settings = settings,
                    .has_cross_site_ancestor = has_cross_site_ancestor,
                    .is_window = true,
                    .agent_type = {},
                });
                return decision;
            });
            return decision;
        });
        return decision;
    });
    if (decision == IterationDecision::Break)
        return;

    WorkerAgentManager::the().for_each_agent_environment([&](WebWorkerClient& client, CanonicalWorkerEnvironmentSettingsObject const& settings, IsPrivate is_private, Web::HTML::AgentType agent_type) {
        if (decision == IterationDecision::Break || agent_type == Web::HTML::AgentType::ServiceWorker || is_private != m_is_private)
            return;
        decision = callback(HostedClient {
            .connection = ServiceWorkerClientConnection { client },
            .settings = settings,
            .has_cross_site_ancestor = settings.has_cross_site_ancestor(),
            .is_window = false,
            .agent_type = agent_type,
        });
    });
}

Optional<ServiceWorkerClientConnection> ServiceWorkerRegistry::connection_of_client(Web::HTML::EnvironmentId const& client_id) const
{
    Optional<ServiceWorkerClientConnection> connection;
    for_each_hosted_client([&](HostedClient const& client) {
        if (client.settings.id() != client_id)
            return IterationDecision::Continue;
        connection = client.connection;
        return IterationDecision::Break;
    });
    return connection;
}

// https://w3c.github.io/ServiceWorker/#notify-controller-change-algorithm
void ServiceWorkerRegistry::notify_controller_change(Web::HTML::EnvironmentId const& client, ServiceWorkerDescriptor const& controller)
{
    // 1. Assert: client is not null.
    // 2. If client is an environment settings object, queue a task to fire an event named controllerchange at the
    //    ServiceWorkerContainer object that client is associated with.
    // NB: Only an environment settings object has a canonical record. The process that hosts it queues the task.
    auto connection = connection_of_client(client);
    if (!connection.has_value())
        return;
    connection->visit_open_connection([&](auto& process) { process.async_service_worker_controller_changed(client, controller); });
}

void ServiceWorkerRegistry::match_reserved_client_of_navigation(ServiceWorkerClientConnection const& connection, CanonicalNavigable const& navigable, NavigationLoader const* navigation, ServiceWorkerRequestId request_id, ServiceWorkerReservedClient const& reserved_client)
{
    // https://w3c.github.io/ServiceWorker/#on-fetch-request-algorithm
    // 9.5. Let storage key be the result of running obtain a storage key given reservedClient.
    // NB: The partition is that of the reserved client that "attempt to populate the history entry's document" creates
    //     for the navigable: its top-level origin is the parent's, and it has a cross-site ancestor if the parent's
    //     active document has one or is of another site.
    Optional<Partition> partition;
    auto origin = reserved_client.creation_url.origin();
    if (auto storage_key = Web::StorageAPI::obtain_a_storage_key(origin); storage_key.has_value()) {
        Optional<URL::Origin> top_level_origin = origin;
        bool has_cross_site_ancestor = false;
        if (!navigable.is_top_level_traversable()) {
            auto const* parent = navigable.parent();
            top_level_origin = parent ? parent->active_document().top_level_origin() : Optional<URL::Origin> {};
            has_cross_site_ancestor = !parent || parent->active_document_has_cross_site_ancestor() || !parent->active_document().origin().is_same_site(origin);
        }
        partition = Partition {
            .storage_key = storage_key.release_value(),
            .top_level_site = top_level_origin.has_value() ? URL::Site::serialize_for_partitioning(*top_level_origin) : Optional<Utf16String> {},
            .has_cross_site_ancestor = has_cross_site_ancestor,
        };
    }

    // NB: A navigation's reserved client is a new environment, whose id no environment has.
    bool names_an_environment = false;
    for_each_hosted_client([&](HostedClient const& client) {
        names_an_environment = client.settings.id() == reserved_client.id;
        return names_an_environment ? IterationDecision::Break : IterationDecision::Continue;
    });
    if (names_an_environment)
        partition = {};

    match_reserved_client(connection, partition, request_id, reserved_client, navigable.id(), navigation);
}

void ServiceWorkerRegistry::match_reserved_client_of_worker(ServiceWorkerClientEnvironment const& client, ServiceWorkerRequestId request_id, ServiceWorkerReservedClient const& reserved_client)
{
    // NB: A worker's reserved client is the worker's own environment settings object.
    match_reserved_client(client.connection, partition_of(client.settings, client.has_cross_site_ancestor), request_id, reserved_client, {}, nullptr);
}

// https://w3c.github.io/ServiceWorker/#on-fetch-request-algorithm
void ServiceWorkerRegistry::match_reserved_client(ServiceWorkerClientConnection const& connection, Optional<Partition> const& partition, ServiceWorkerRequestId request_id, ServiceWorkerReservedClient const& reserved_client, Optional<Web::HTML::CrossProcessId> reserving_navigable, NavigationLoader const* reserving_navigation)
{
    auto reply = [&](Optional<ServiceWorkerControllerMatch> const& match) {
        connection.visit_open_connection([&](auto& client) { client.async_service_worker_match_finished(request_id, match); });
    };

    // AD-HOC: Obtaining a storage key can fail, which Handle Fetch does not expect. A request is of its reserved
    //         client's origin, as a navigation discards a reserved client of another origin.
    if (!partition.has_value() || !reserved_client.url.origin().is_same_origin(partition->storage_key.origin)) {
        reply({});
        return;
    }

    // NB: A navigable has one navigation at a time, which discards a reserved client of another origin than the one its
    //     fetch redirects to. Only the newest reserved client of a navigable is its navigation's.
    if (reserving_navigable.has_value())
        forget_reserved_clients_of_navigable(*reserving_navigable, reserved_client.id);

    // 9.6. Set registration to the result of running Match Service Worker Registration given storage key and request's
    //      url.
    auto* registration = match_service_worker_registration(*partition, reserved_client.url);

    // 9.7. If registration is null or registration's active worker is null, return null.
    if (!registration || !registration->active_worker.has_value()) {
        reply({});
        return;
    }
    auto& active_worker = **m_service_workers.get(*registration->active_worker);

    // 9.8. If request's destination is not "report", set reservedClient's active service worker to registration's
    //      active worker.
    // NB: Nothing fetches with the "report" destination.
    auto existing_client = m_controlled_clients.find_if([&](ControlledClient const& client) { return client.id == reserved_client.id; });
    if (existing_client != m_controlled_clients.end()) {
        // NB: A reserved client is the reserved client of one navigable's navigation only.
        if (existing_client->reserving_navigable != reserving_navigable) {
            reply({});
            return;
        }
        existing_client->active_service_worker = active_worker.id;
    } else {
        m_controlled_clients.append(ControlledClient {
            .id = reserved_client.id,
            .active_service_worker = active_worker.id,
            .reserving_navigable = reserving_navigable,
            .reserving_navigation = reserving_navigation,
            .reserving_process = reserving_navigable.has_value() ? connection.identity() : nullptr,
        });
    }

    // NOTE: From this point, the service worker client starts to use its active service worker's containing service
    //       worker registration.
    reply(ServiceWorkerControllerMatch {
        .active_worker = *descriptor_of(active_worker.id),
        .registration = snapshot_of(*registration),
        .handles_fetch_events = active_worker.set_of_event_types_to_handle.contains_slow("fetch"sv),
    });
}

static bool is_reserved_client_of(auto const& client, NavigationLoader const& navigation, Web::HTML::EnvironmentId const& reserved_client_id)
{
    return client.id == reserved_client_id
        && client.reserving_navigable == navigation.request().navigable_id
        && (!client.reserving_navigation || client.reserving_navigation == &navigation);
}

bool ServiceWorkerRegistry::has_reserved_client_of_navigation(NavigationLoader const& navigation, Web::HTML::EnvironmentId const& reserved_client_id)
{
    return any_of(live_registries(), [&](ServiceWorkerRegistry* registry) {
        return any_of(registry->m_controlled_clients, [&](ControlledClient const& client) {
            return is_reserved_client_of(client, navigation, reserved_client_id);
        });
    });
}

Optional<ServiceWorkerDescriptor> ServiceWorkerRegistry::reserved_client_became_window(NavigationLoader const& navigation, Web::HTML::EnvironmentId const& reserved_client_id, Web::HTML::EnvironmentId const& window_id)
{
    for (auto* registry : live_registries()) {
        for (auto& client : registry->m_controlled_clients) {
            if (!is_reserved_client_of(client, navigation, reserved_client_id))
                continue;

            // https://html.spec.whatwg.org/multipage/nav-history-apis.html#set-up-a-window-environment-settings-object
            // 4.1. Set settings object's id to reservedEnvironment's id, target browsing context to reservedEnvironment's
            //      target browsing context, and active service worker to reservedEnvironment's active service worker.
            // NB: A navigation that reuses the initial about:blank document's window keeps that window's id.
            auto navigable_id = *client.reserving_navigable;
            auto active_service_worker = client.active_service_worker;
            client.id = window_id;
            client.reserving_navigable = {};
            client.reserving_navigation = nullptr;
            client.reserving_process = nullptr;
            registry->forget_reserved_clients_of_navigable(navigable_id, window_id);
            return registry->descriptor_of(active_service_worker);
        }
    }
    return {};
}

void ServiceWorkerRegistry::navigation_loader_will_be_destroyed(NavigationLoader const& navigation)
{
    for (auto* registry : live_registries())
        registry->forget_reserved_clients_of(navigation);
}

// NB: A reserved client that no window took over is discarded with its navigation.
void ServiceWorkerRegistry::forget_reserved_clients_of(NavigationLoader const& navigation)
{
    Vector<Web::HTML::EnvironmentId> reserved_clients;
    for (auto const& client : m_controlled_clients) {
        if (client.is_reserved() && client.reserving_navigation == &navigation)
            reserved_clients.append(client.id);
    }
    for (auto const& reserved_client : reserved_clients)
        handle_service_worker_client_unload(reserved_client);
}

void ServiceWorkerRegistry::forget_reserved_clients_of_navigable(Web::HTML::CrossProcessId navigable_id, Optional<Web::HTML::EnvironmentId const&> except)
{
    Vector<Web::HTML::EnvironmentId> reserved_clients;
    for (auto const& client : m_controlled_clients) {
        if (client.reserving_navigable == navigable_id && client.id != except)
            reserved_clients.append(client.id);
    }
    for (auto const& reserved_client : reserved_clients)
        handle_service_worker_client_unload(reserved_client);
}

Optional<ServiceWorkerRegistry::Partition> ServiceWorkerRegistry::partition_of(CanonicalEnvironmentSettingsObject const& settings, bool has_cross_site_ancestor)
{
    auto storage_key = obtain_a_storage_key(settings);
    if (!storage_key.has_value())
        return {};
    return Partition {
        .storage_key = storage_key.release_value(),
        .top_level_site = network_isolation_top_level_site(settings),
        .has_cross_site_ancestor = has_cross_site_ancestor,
    };
}

ServiceWorkerRegistry::RegistrationKey ServiceWorkerRegistry::registration_key(Partition const& partition, URL::URL const& scope_url)
{
    return RegistrationKey {
        .partition = partition,
        .serialized_scope_url = scope_url.serialize(URL::ExcludeFragment::Yes),
    };
}

// https://w3c.github.io/ServiceWorker/#dfn-job-equivalent
bool ServiceWorkerRegistry::Job::is_equivalent_to(Job const& other) const
{
    // Two jobs are equivalent when their job type is the same and:
    if (job_type != other.job_type)
        return false;

    switch (job_type) {
    // For register and update jobs, their scope url, script url, worker type, and update via cache mode are the same.
    case ServiceWorkerJobType::Register:
    case ServiceWorkerJobType::Update:
        return scope_url == other.scope_url
            && script_url == other.script_url
            && worker_type == other.worker_type
            && update_via_cache == other.update_via_cache;
    // For unregister jobs, their scope url is the same.
    case ServiceWorkerJobType::Unregister:
        return scope_url == other.scope_url;
    }
    VERIFY_NOT_REACHED();
}

// https://w3c.github.io/ServiceWorker/#schedule-job-algorithm
void ServiceWorkerRegistry::schedule_job(ServiceWorkerClientEnvironment const& client, ServiceWorkerJobRequest request)
{
    // NB: The client's process runs the steps of register() up to the creation of its job, which is created here,
    //     from the client's environment as this process knows it.
    auto partition = partition_of(client.settings, client.has_cross_site_ancestor);
    if (!partition.has_value()) {
        // AD-HOC: Obtaining a storage key can fail, which the job algorithms do not expect.
        client.connection.service_worker_job_settled(request.request_id, ServiceWorkerJobError::TypeError);
        return;
    }

    request.client.origin = client.settings.origin();
    request.client.top_level_origin = client.settings.top_level_origin();
    request.client.has_cross_site_ancestor = client.has_cross_site_ancestor;

    // https://w3c.github.io/ServiceWorker/#create-job-algorithm
    // 1. Let job be a new job.
    // 2. Set job's job type to jobType.
    // 3. Set job's storage key to storage key.
    // 4. Set job's scope url to scopeURL.
    // 5. Set job's script url to scriptURL.
    // 6. Set job's job promise to promise.
    // 7. Set job's client to client.
    // NB: The client's process holds the promise, which settles once the client learns how the job settled.
    // 8. If client is not null, set job's referrer to client's creation URL.
    // AD-HOC: Register compares only the referrer's origin, so the job keeps the origin of its client as this process
    //         knows it, rather than a creation URL that the client's process names.
    // 9. Return job.
    auto job = make<Job>(Job {
        .id = ++m_next_job_id,
        .job_type = request.job_type,
        .partition = partition.release_value(),
        .scope_url = move(request.scope_url),
        .script_url = move(request.script_url),
        .worker_type = request.worker_type,
        .update_via_cache = request.update_via_cache,
        .client = JobClient {
            .connection = client.connection,
            .environment_id = client.settings.id(),
            .request_id = request.request_id,
            .settings = move(request.client),
            .has_dispatched_dom_content_loaded = request.client_has_dispatched_dom_content_loaded,
        },
        .referrer_origin = client.settings.origin(),
        .force_bypass_cache = false,
        .list_of_equivalent_jobs = {},
        .job_promise_has_settled = false,
        .has_started = false,
    });

    // 1. Let jobQueue be null.
    // 2. Let jobScope be job's scope url, serialized.
    auto job_scope = registration_key(job->partition, job->scope_url);

    // 3. If scope to job queue map[jobScope] does not exist, set scope to job queue map[jobScope] to a new job queue.
    // 4. Set jobQueue to scope to job queue map[jobScope].
    auto& job_queue = m_scope_to_job_queue_map.ensure(job_scope);

    // 5. If jobQueue is empty, then:
    if (job_queue.is_empty()) {
        // 1. Set job's containing job queue to jobQueue, and enqueue job to jobQueue.
        job_queue.append(move(job));

        // 2. Invoke Run Job with jobQueue.
        run_job(job_scope);
        return;
    }

    // 6. Else:
    // 1. Let lastJob be the element at the back of jobQueue.
    auto& last_job = *job_queue.last();

    // 2. If job is equivalent to lastJob and lastJob's job promise has not settled, append job to lastJob's list of
    //    equivalent jobs.
    if (job->is_equivalent_to(last_job) && !last_job.job_promise_has_settled) {
        last_job.list_of_equivalent_jobs.append(move(job));
        return;
    }

    // 3. Else, set job's containing job queue to jobQueue, and enqueue job to jobQueue.
    job_queue.append(move(job));
}

// https://w3c.github.io/ServiceWorker/#run-job-algorithm
void ServiceWorkerRegistry::run_job(RegistrationKey const& job_queue_key)
{
    // 1. Assert: jobQueue is not empty.
    VERIFY(!m_scope_to_job_queue_map.get(job_queue_key)->is_empty());

    // 2. Queue a task to run these steps:
    Core::deferred_invoke([weak_this = make_weak_ptr(), job_queue_key] {
        auto* registry = weak_this.ptr();
        if (!registry)
            return;
        auto job_queue = registry->m_scope_to_job_queue_map.get(job_queue_key);
        if (!job_queue.has_value() || job_queue->is_empty())
            return;

        // 1. Let job be the first item in jobQueue.
        auto& job = *job_queue->first();
        if (job.has_started)
            return;

        switch (job.job_type) {
        // 2. If job's job type is register, run Register with job in parallel.
        case ServiceWorkerJobType::Register:
        // 3. Else if job's job type is update, run Update with job in parallel.
        case ServiceWorkerJobType::Update:
            // NOTE: For a register job and an update job, the user agent delays queuing a task for running the job until
            //       after a DOMContentLoaded event has been dispatched to the document that initiated the job.
            // NB: Jobs behind one that waits wait with it. Once its client reports DOMContentLoaded or is discarded,
            //     client_did_dispatch_dom_content_loaded() runs the job.
            if (job.client.has_value() && !job.client->has_dispatched_dom_content_loaded)
                return;
            job.has_started = true;
            if (job.job_type == ServiceWorkerJobType::Register)
                registry->register_(job_queue_key, job);
            else
                registry->update(job_queue_key, job);
            break;

        // 4. Else if job's job type is unregister, run Unregister with job in parallel.
        case ServiceWorkerJobType::Unregister:
            job.has_started = true;
            registry->unregister(job_queue_key, job);
            break;
        }
    });
}

void ServiceWorkerRegistry::client_did_dispatch_dom_content_loaded(Web::HTML::EnvironmentId const& environment_id)
{
    Vector<RegistrationKey> job_queues_to_run;
    for (auto& [job_queue_key, job_queue] : m_scope_to_job_queue_map) {
        for (auto& job : job_queue) {
            if (job->client.has_value() && job->client->environment_id == environment_id)
                job->client->has_dispatched_dom_content_loaded = true;
        }
        if (!job_queue.is_empty() && !job_queue.first()->has_started)
            job_queues_to_run.append(job_queue_key);
    }

    for (auto const& job_queue_key : job_queues_to_run)
        run_job(job_queue_key);
}

// https://w3c.github.io/ServiceWorker/#finish-job-algorithm
void ServiceWorkerRegistry::finish_job(RegistrationKey const& job_queue_key, Job& job)
{
    // 1. Let jobQueue be job's containing job queue.
    auto& job_queue = *m_scope_to_job_queue_map.get(job_queue_key);

    // 2. Assert: the first item in jobQueue is job.
    VERIFY(job_queue.first().ptr() == &job);

    // 3. Dequeue from jobQueue.
    (void)job_queue.take_first();

    // 4. If jobQueue is not empty, invoke Run Job with jobQueue.
    if (!job_queue.is_empty())
        run_job(job_queue_key);
    else
        m_scope_to_job_queue_map.remove(job_queue_key);
}

void ServiceWorkerRegistry::settle_job_promise(Job& job, ServiceWorkerJobResult const& result)
{
    if (job.job_promise_has_settled)
        return;
    job.job_promise_has_settled = true;
    if (job.client.has_value())
        job.client->connection.service_worker_job_settled(job.client->request_id, result);
}

// https://w3c.github.io/ServiceWorker/#resolve-job-promise-algorithm
void ServiceWorkerRegistry::resolve_job_promise(Job& job, ServiceWorkerJobResult const& value)
{
    // 1. If job's client is not null, queue a task, on job's client's responsible event loop using the DOM manipulation
    //    task source, to run the following substeps:
    // NB: The client's process queues the task, and runs its substeps, when it learns that the job promise settled.
    settle_job_promise(job, value);

    // 2. For each equivalentJob in job's list of equivalent jobs:
    for (auto& equivalent_job : job.list_of_equivalent_jobs) {
        // 1. If equivalentJob's client is null, continue to the next iteration of the loop.
        // 2. Queue a task, on equivalentJob's client's responsible event loop using the DOM manipulation task source, to
        //    run the following substeps:
        settle_job_promise(*equivalent_job, value);
    }
}

// https://w3c.github.io/ServiceWorker/#reject-job-promise-algorithm
void ServiceWorkerRegistry::reject_job_promise(Job& job, ServiceWorkerJobError error_data)
{
    // 1. If job's client is not null, queue a task, on job's client's responsible event loop using the DOM manipulation
    //    task source, to reject job's job promise with a new exception with errorData, in job's client's Realm.
    settle_job_promise(job, error_data);

    // 2. For each equivalentJob in job's list of equivalent jobs:
    for (auto& equivalent_job : job.list_of_equivalent_jobs) {
        // 1. If equivalentJob's client is null, continue.
        // 2. Queue a task, on equivalentJob's client's responsible event loop using the DOM manipulation task source, to
        //    reject equivalentJob's job promise with a new exception with errorData, in equivalentJob's client's Realm.
        settle_job_promise(*equivalent_job, error_data);
    }
}

// https://w3c.github.io/ServiceWorker/#register-algorithm
void ServiceWorkerRegistry::register_(RegistrationKey const& job_queue_key, Job& job)
{
    auto script_origin = job.script_url->origin();
    auto scope_origin = job.scope_url.origin();

    // 1. If the result of running potentially trustworthy origin with the origin of job's script url as the argument is
    //    Not Trusted, then:
    if (Web::SecureContexts::is_origin_potentially_trustworthy(script_origin) == Web::SecureContexts::Trustworthiness::NotTrustworthy) {
        // 1. Invoke Reject Job Promise with job and "SecurityError" DOMException.
        reject_job_promise(job, ServiceWorkerJobError::SecurityError);

        // 2. Invoke Finish Job with job and abort these steps.
        finish_job(job_queue_key, job);
        return;
    }

    // 2. If job's script url's origin and job's referrer's origin are not same origin, then:
    if (!script_origin.is_same_origin(job.referrer_origin)) {
        // 1. Invoke Reject Job Promise with job and "SecurityError" DOMException.
        reject_job_promise(job, ServiceWorkerJobError::SecurityError);

        // 2. Invoke Finish Job with job and abort these steps.
        finish_job(job_queue_key, job);
        return;
    }

    // 3. If job's scope url's origin and job's referrer's origin are not same origin, then:
    if (!scope_origin.is_same_origin(job.referrer_origin)) {
        // 1. Invoke Reject Job Promise with job and "SecurityError" DOMException.
        reject_job_promise(job, ServiceWorkerJobError::SecurityError);

        // 2. Invoke Finish Job with job and abort these steps.
        finish_job(job_queue_key, job);
        return;
    }

    // 4. Let registration be the result of running Get Registration given job's storage key and job's scope url.
    auto* registration = get_registration(job.partition, job.scope_url);

    // 5. If registration is not null, then:
    if (registration) {
        // 1. Let newestWorker be the result of running the Get Newest Worker algorithm passing registration as the
        //    argument.
        auto newest_worker = get_newest_worker(*registration);

        // 2. If newestWorker is not null, job's script url equals newestWorker's script url, job's worker type equals
        //    newestWorker's type, and job's update via cache mode's value equals registration's update via cache mode,
        //    then:
        if (newest_worker.has_value()) {
            auto const& worker = m_service_workers.get(*newest_worker).value();
            if (job.script_url == worker->script_url && job.worker_type == worker->type && job.update_via_cache == registration->update_via_cache) {
                // 1. Invoke Resolve Job Promise with job and registration.
                resolve_job_promise(job, snapshot_of(*registration));

                // 2. Invoke Finish Job with job and abort these steps.
                finish_job(job_queue_key, job);
                return;
            }
        }
    }
    // 6. Else:
    else {
        // 1. Invoke Set Registration algorithm with job's storage key, job's scope url, and job's update via cache mode.
        set_registration(job);
    }

    // 7. Invoke Update algorithm passing job as the argument.
    update(job_queue_key, job);
}

// https://w3c.github.io/ServiceWorker/#update-algorithm
void ServiceWorkerRegistry::update(RegistrationKey const& job_queue_key, Job& job)
{
    // 1. Let registration be the result of running Get Registration given job's storage key and job's scope url.
    auto* registration = get_registration(job.partition, job.scope_url);

    // 2. If registration is null, then:
    if (!registration) {
        // 1. Invoke Reject Job Promise with job and TypeError.
        reject_job_promise(job, ServiceWorkerJobError::TypeError);

        // 2. Invoke Finish Job with job and abort these steps.
        finish_job(job_queue_key, job);
        return;
    }

    // 3. Let newestWorker be the result of running Get Newest Worker algorithm passing registration as the argument.
    auto newest_worker = get_newest_worker(*registration);

    // 4. If job's job type is update, and newestWorker is not null and its script url does not equal job's script url,
    //    then:
    if (job.job_type == ServiceWorkerJobType::Update && newest_worker.has_value() && m_service_workers.get(*newest_worker).value()->script_url != job.script_url) {
        // 1. Invoke Reject Job Promise with job and TypeError.
        reject_job_promise(job, ServiceWorkerJobError::TypeError);

        // 2. Invoke Finish Job with job and abort these steps.
        finish_job(job_queue_key, job);
        return;
    }

    // 5. Let hasUpdatedResources be false.
    // 6. Let updatedResourceMap be an ordered map where the keys are URLs and the values are responses.
    // 7. Switching on job's worker type, run these substeps with the following options:
    // NB: The service worker this creates runs in a process of its own, which runs these steps and the perform the
    //     fetch hook with the environment settings object it creates for the worker. It reports how they end to
    //     service_worker_did_fetch_script(), and then, once it has run the script, to service_worker_did_run().
    auto fail_to_start = [&] {
        reject_job_promise(job, ServiceWorkerJobError::TypeError);
        if (!newest_worker.has_value())
            remove_registration(*registration);
        finish_job(job_queue_key, job);
    };

    if (!job.client.has_value()) {
        fail_to_start();
        return;
    }

    auto agent = WorkerAgentManager::the().start_service_worker_agent(m_is_private, job.client->settings.origin, job.client->settings.top_level_origin, job.client->settings.has_cross_site_ancestor);
    if (agent.is_error()) {
        // NB: A service worker the system cannot give a process fails, as one whose script cannot be fetched does.
        dbgln("Unable to start a service worker process: {}", agent.error());
        fail_to_start();
        return;
    }

    auto worker_id = ++m_next_service_worker_id;
    auto worker = make<ServiceWorkerRecord>(ServiceWorkerRecord {
        .id = worker_id,
        .registration_id = registration->id,
        .partition = registration->key.partition,
        .script_url = *job.script_url,
        .type = job.worker_type,
        .state = ServiceWorkerState::Parsed,
        .set_of_event_types_to_handle = {},
        .skip_waiting_flag = false,
        .agent_id = agent.value().id,
        .process = agent.value().client,
        .awaited_step = AwaitedStep::Start,
        .job_in_progress = JobInProgress {
            .job_id = job.id,
            .job_queue_key = job_queue_key,
            .newest_worker = newest_worker,
        },
        .deadline_timer = {},
        .registering_client = job.client->settings,
        .script_resources = {},
        .messages_waiting_for_run = {},
        .heartbeat_timer = {},
        .unanswered_heartbeat = {},
    });

    ServiceWorkerStartRequest start_request {
        .worker = ServiceWorkerDescriptor {
            .id = worker->id,
            .script_url = worker->script_url,
            .state = worker->state,
        },
        .worker_type = worker->type,
        .registration = snapshot_of(*registration),
        .environment_id = agent.value().environment_id,
        .origin = job.client->settings.origin,
        .has_cross_site_ancestor = job.client->settings.has_cross_site_ancestor,
        .top_level_origin = job.client->settings.top_level_origin,
        .script_fetch = ServiceWorkerScriptFetchParameters {
            .job_client = job.client->settings,
            .force_bypass_cache = job.force_bypass_cache,
        },
        .stored_scripts = {},
        .extendable_event_timeout_ms = static_cast<u64>(lifecycle_event_deadline().to_milliseconds()),
    };
    agent.value().client->async_start_service_worker(start_request);

    worker->deadline_timer = Core::Timer::create_single_shot(new_service_worker_start_deadline, [weak_this = make_weak_ptr(), worker_id] {
        auto* registry = weak_this.ptr();
        if (!registry)
            return;
        if (auto worker = registry->m_service_workers.get(worker_id); worker.has_value())
            registry->terminate_service_worker(**worker);
    });
    worker->deadline_timer->start();
    start_heartbeat(*worker);

    m_service_workers.set(worker_id, move(worker));
}

ServiceWorkerRegistry::Job* ServiceWorkerRegistry::job_in_progress_of(ServiceWorkerRecord const& worker)
{
    if (!worker.job_in_progress.has_value())
        return nullptr;
    auto job_queue = m_scope_to_job_queue_map.get(worker.job_in_progress->job_queue_key);
    VERIFY(job_queue.has_value() && !job_queue->is_empty());
    auto& job = *job_queue->first();
    VERIFY(job.id == worker.job_in_progress->job_id);
    return &job;
}

// https://w3c.github.io/ServiceWorker/#update-algorithm
void ServiceWorkerRegistry::fail_update(ServiceWorkerRecord& worker, ServiceWorkerJobError error)
{
    auto job_in_progress = worker.job_in_progress.release_value();
    worker.awaited_step = AwaitedStep::None;
    terminate_service_worker(worker);

    auto job_queue = m_scope_to_job_queue_map.get(job_in_progress.job_queue_key);
    auto& job = *job_queue->first();
    auto* registration = registration_by_id(worker.registration_id);

    // 8. If script is null or Is Async Module with script's record, script's base URL, and « » is true, then:
    // 1. Invoke Reject Job Promise with job and TypeError.
    // NOTE: This will do nothing if Reject Job Promise was previously invoked with "SecurityError" DOMException.
    // 16. If runResult is failure or an abrupt completion, then:
    // 1. Invoke Reject Job Promise with job and TypeError.
    // NB: The worker's process reports the exception the job promise was first rejected with.
    reject_job_promise(job, error);

    // 2. If newestWorker is null, then remove registration map[(registration's storage key, serialized scopeURL)].
    if (!job_in_progress.newest_worker.has_value() && registration)
        remove_registration(*registration);

    // 3. Invoke Finish Job with job and abort these steps.
    forget_service_worker_if_unused(worker.id);
    finish_job(job_in_progress.job_queue_key, job);
}

bool ServiceWorkerRegistry::is_service_worker_agent(Web::HTML::WorkerAgentId agent_id) const
{
    for (auto const& [id, worker] : m_service_workers) {
        if (worker->agent_id == agent_id)
            return true;
    }
    return false;
}

ServiceWorkerRegistry::ServiceWorkerRecord* ServiceWorkerRegistry::service_worker_of_agent(Web::HTML::WorkerAgentId agent_id)
{
    for (auto& [id, worker] : m_service_workers) {
        if (worker->agent_id == agent_id)
            return worker.ptr();
    }
    return nullptr;
}

void ServiceWorkerRegistry::service_worker_did_fetch_script(Web::HTML::WorkerAgentId agent_id, ServiceWorkerScriptFetchResult result)
{
    auto* worker = service_worker_of_agent(agent_id);
    if (!worker || worker->awaited_step != AwaitedStep::Start || !result.has_value())
        return;
    fail_update(*worker, *result);
}

// https://w3c.github.io/ServiceWorker/#update-algorithm
void ServiceWorkerRegistry::service_worker_did_run(Web::HTML::WorkerAgentId agent_id, bool succeeded, Vector<String> event_types_to_handle)
{
    auto* worker = service_worker_of_agent(agent_id);
    if (worker && worker->awaited_step == AwaitedStep::Restart) {
        worker->deadline_timer->stop();

        // https://w3c.github.io/ServiceWorker/#run-service-worker-algorithm
        // 11. If startFailed is true, then return failure.
        if (!succeeded) {
            terminate_service_worker(*worker);
            return;
        }

        // 12. Return serviceWorker's start status.
        // NB: The messages that waited for the worker to run reach it now.
        worker->awaited_step = AwaitedStep::None;
        for (auto& waiting_message : exchange(worker->messages_waiting_for_run, {}))
            worker->process->async_dispatch_service_worker_message_event(move(waiting_message.source), waiting_message.origin, move(waiting_message.message), lifecycle_event_deadline().to_milliseconds());
        stop_workers_of_partitions_without_clients();
        return;
    }
    if (!worker || worker->awaited_step != AwaitedStep::Start)
        return;
    worker->deadline_timer->stop();

    // NB: Only the test-web reset removes a registration while its job runs.
    if (!registration_by_id(worker->registration_id)) {
        fail_update(*worker, ServiceWorkerJobError::TypeError);
        return;
    }

    // FIXME: 9. If hasUpdatedResources is false, then:
    // FIXME:    1. Set registration's update via cache mode to job's update via cache mode.
    // FIXME:    2. Invoke Resolve Job Promise with job and registration.
    // FIXME:    3. Invoke Finish Job with job and abort these steps.

    // 10. Let worker be a new service worker.
    // 11. Set worker's script url to job's script url, worker's script resource to script, worker's type to job's worker
    //     type, and worker's script resource map to updatedResourceMap.
    // 12. Append url to worker's set of used scripts.
    // 13. Set worker's script resource's policy container to policyContainer.
    // NB: The worker's process holds its script resource and script resource map.

    // 14. Let forceBypassCache be true if job's force bypass cache flag is set, and false otherwise.
    // 15. Let runResult be the result of running the Run Service Worker algorithm with worker and forceBypassCache.
    // NB: The worker's process runs the worker once its script is fetched, and reports the result here.

    // 16. If runResult is failure or an abrupt completion, then:
    if (!succeeded) {
        fail_update(*worker, ServiceWorkerJobError::TypeError);
        return;
    }

    // https://w3c.github.io/ServiceWorker/#run-service-worker-algorithm
    // 9.8.1.1. Append eventType to workerGlobalScope's associated service worker's set of event types to handle.
    worker->set_of_event_types_to_handle = move(event_types_to_handle);

    // 17. Else, invoke Install algorithm with job, worker, and registration as its arguments.
    install(*worker);
}

// https://w3c.github.io/ServiceWorker/#should-skip-event-algorithm
static bool should_skip_event(StringView event_name, Vector<String> const& set_of_event_types_to_handle)
{
    // 1. If serviceWorker's set of event types to handle does not contain eventName, then the user agent may return
    //    true.
    if (!set_of_event_types_to_handle.contains_slow(event_name))
        return true;

    // 2. Return false.
    return false;
}

// https://w3c.github.io/ServiceWorker/#service-worker-has-no-pending-events
bool ServiceWorkerRegistry::service_worker_has_no_pending_events(Optional<ServiceWorkerId> worker_id) const
{
    // NB: A registration without the worker has none to wait for.
    if (!worker_id.has_value())
        return true;
    auto worker = m_service_workers.get(*worker_id);

    // 1. For each event of worker's set of extended events:
    //    1. If event is active, return false.
    // NB: The worker's process reports whether its set holds an active event.
    if (worker.has_value() && (*worker)->has_pending_events)
        return false;

    // 2. Return true.
    return true;
}

// https://w3c.github.io/ServiceWorker/#installation-algorithm
void ServiceWorkerRegistry::install(ServiceWorkerRecord& worker)
{
    auto& job = *job_in_progress_of(worker);
    auto& registration = *registration_by_id(worker.registration_id);

    // 1. Let installFailed be false.
    // 2. Let newestWorker be the result of running Get Newest Worker algorithm passing registration as its argument.
    // NB: Update already took the newest worker, which it holds with the job.

    // 3. Set registration's update via cache mode to job's update via cache mode.
    registration.update_via_cache = job.update_via_cache;

    // 4. Run the Update Registration State algorithm passing registration, "installing" and worker as the arguments.
    update_registration_state(registration, ServiceWorkerRegistrationSlot::Installing, worker.id);

    // 5. Run the Update Worker State algorithm passing registration's installing worker and "installing" as the
    //    arguments.
    update_worker_state(worker, ServiceWorkerState::Installing);

    // 6. Assert: job's job promise is not null.
    // 7. Invoke Resolve Job Promise with job and registration.
    resolve_job_promise(job, snapshot_of(registration));

    // 8. Let settingsObjects be all environment settings objects whose origin is registration's scope url's origin.
    // 9. For each settingsObject of settingsObjects, queue a task on settingsObject's responsible event loop in the DOM
    //    manipulation task source to run the following steps:
    // NB: The processes that host environments in the registration's partition run the task's steps.
    for_each_process_in_partition(registration.key.partition, [&](ServiceWorkerClientConnection const& connection) {
        connection.visit_open_connection([&](auto& client) { client.async_service_worker_update_found(registration.id); });
    });

    // 10. Let installingWorker be registration's installing worker.
    // 11. If the result of running the Should Skip Event algorithm with installingWorker and "install" is false, then:
    if (!should_skip_event("install"sv, worker.set_of_event_types_to_handle)) {
        // 1. Let forceBypassCache be true if job's force bypass cache flag is set, and false otherwise.
        // 2. If the result of running the Run Service Worker algorithm with installingWorker and forceBypassCache is
        //    failure, then:
        //    1. Set installFailed to true.
        // NB: The worker runs, as it ran in Update.

        // 3. Else:
        // 1. Queue a task task on installingWorker's event loop using the DOM manipulation task source to run the
        //    following steps:
        // 2. Wait for task to have executed or been discarded.
        // 3. Wait for the step labeled WaitForAsynchronousExtensions to complete.
        // NB: The worker's process runs the task, and reports to service_worker_did_finish_lifecycle_event() whether
        //     installFailed is true once the step labeled WaitForAsynchronousExtensions completes.
        dispatch_lifecycle_event(worker, ServiceWorkerLifecycleEvent::Install);
        return;
    }

    finish_install(worker, false);
}

// https://w3c.github.io/ServiceWorker/#installation-algorithm
void ServiceWorkerRegistry::finish_install(ServiceWorkerRecord& worker, bool install_failed)
{
    auto job_in_progress = worker.job_in_progress.release_value();
    worker.awaited_step = AwaitedStep::None;
    auto job_queue = m_scope_to_job_queue_map.get(job_in_progress.job_queue_key);
    auto& job = *job_queue->first();
    auto& registration = *registration_by_id(worker.registration_id);

    // 12. If installFailed is true, then:
    if (install_failed) {
        // 1. Run the Update Worker State algorithm passing registration's installing worker and "redundant" as the
        //    arguments.
        update_worker_state(worker, ServiceWorkerState::Redundant);
        terminate_service_worker(worker);

        // 2. Run the Update Registration State algorithm passing registration, "installing" and null as the arguments.
        update_registration_state(registration, ServiceWorkerRegistrationSlot::Installing, {});

        // 3. If newestWorker is null, then remove registration map[(registration's storage key, serialized
        //    registration's scope url)].
        if (!job_in_progress.newest_worker.has_value())
            remove_registration(registration);

        // 4. Invoke Finish Job with job and abort these steps.
        forget_service_worker_if_unused(worker.id);
        finish_job(job_in_progress.job_queue_key, job);
        return;
    }

    // 13. Let map be registration's installing worker's script resource map.
    // 14. Let usedSet be registration's installing worker's set of used scripts.
    // 15. For each url of map:
    //     1. If usedSet does not contain url, then remove map[url].
    // NB: The worker's process holds the map and the set, and prunes the map once the install event has finished.

    // 16. If registration's waiting worker is not null, then:
    if (registration.waiting_worker.has_value()) {
        auto& waiting_worker = **m_service_workers.get(*registration.waiting_worker);

        // 1. Terminate registration's waiting worker.
        terminate_service_worker(waiting_worker);

        // 2. Run the Update Worker State algorithm passing registration's waiting worker and "redundant" as the
        //    arguments.
        update_worker_state(waiting_worker, ServiceWorkerState::Redundant);
    }
    auto previous_waiting_worker = registration.waiting_worker;

    // 17. Run the Update Registration State algorithm passing registration, "waiting" and registration's installing
    //     worker as the arguments.
    update_registration_state(registration, ServiceWorkerRegistrationSlot::Waiting, registration.installing_worker);

    // 18. Run the Update Registration State algorithm passing registration, "installing" and null as the arguments.
    update_registration_state(registration, ServiceWorkerRegistrationSlot::Installing, {});

    // 19. Run the Update Worker State algorithm passing registration's waiting worker and "installed" as the arguments.
    update_worker_state(worker, ServiceWorkerState::Installed);
    if (previous_waiting_worker.has_value())
        forget_service_worker_if_unused(*previous_waiting_worker);

    // 20. Invoke Finish Job with job.
    finish_job(job_in_progress.job_queue_key, job);

    // 21. Wait for all the tasks queued by Update Worker State invoked in this algorithm to have executed.
    // NB: Each process queues the tasks of the state changes it is told of in the order it is told of them.

    // 22. Invoke Try Activate with registration.
    // NOTE: If Try Activate does not trigger Activate here, Activate is tried again when the last client controlled by
    //       the existing active worker is unloaded, skipWaiting() is asynchronously called, or the extend lifetime
    //       promises for the existing active worker settle.
    try_activate(registration);
}

// https://w3c.github.io/ServiceWorker/#try-activate-algorithm
void ServiceWorkerRegistry::try_activate(Registration& registration)
{
    // 1. If registration's waiting worker is null, return.
    if (!registration.waiting_worker.has_value())
        return;

    // 2. If registration's active worker is not null and registration's active worker's state is "activating", return.
    // NOTE: If the existing active worker is still in activating state, the activation of the waiting worker is
    //       delayed.
    if (registration.active_worker.has_value() && m_service_workers.get(*registration.active_worker).value()->state == ServiceWorkerState::Activating)
        return;

    // 3. Invoke Activate with registration if either of the following is true:
    //    - registration's active worker is null.
    //    - The result of running Service Worker Has No Pending Events with registration's active worker is true, and
    //      no service worker client is using registration or registration's waiting worker's skip waiting flag is set.
    auto const& waiting_worker = **m_service_workers.get(*registration.waiting_worker);
    if (!registration.active_worker.has_value()
        || (service_worker_has_no_pending_events(registration.active_worker) && (!any_service_worker_client_is_using(registration) || waiting_worker.skip_waiting_flag)))
        activate(registration);
}

// https://w3c.github.io/ServiceWorker/#activation-algorithm
void ServiceWorkerRegistry::activate(Registration& registration)
{
    // 1. If registration's waiting worker is null, abort these steps.
    if (!registration.waiting_worker.has_value())
        return;

    // 2. If registration's active worker is not null, then:
    auto previous_active_worker = registration.active_worker;
    if (previous_active_worker.has_value()) {
        auto& active_worker = **m_service_workers.get(*previous_active_worker);

        // 1. Terminate registration's active worker.
        terminate_service_worker(active_worker);

        // 2. Run the Update Worker State algorithm passing registration's active worker and "redundant" as the
        //    arguments.
        update_worker_state(active_worker, ServiceWorkerState::Redundant);
    }

    // 3. Run the Update Registration State algorithm passing registration, "active" and registration's waiting worker as
    //    the arguments.
    update_registration_state(registration, ServiceWorkerRegistrationSlot::Active, registration.waiting_worker);

    // 4. Run the Update Registration State algorithm passing registration, "waiting" and null as the arguments.
    update_registration_state(registration, ServiceWorkerRegistrationSlot::Waiting, {});

    // 5. Run the Update Worker State algorithm passing registration's active worker and "activating" as the arguments.
    // NOTE: Once an active worker is activating, neither a runtime script error nor a force termination of the active
    //       worker prevents the active worker from getting activated.
    auto& active_worker = **m_service_workers.get(*registration.active_worker);
    update_worker_state(active_worker, ServiceWorkerState::Activating);

    // 6. Let matchedClients be a list of service worker clients whose creation URL matches registration's storage key
    //    and registration's scope url.
    // 7. For each client of matchedClients, queue a task on client's responsible event loop, using the DOM manipulation
    //    task source, to run the following substeps:
    //    1. Let readyPromise be client's global object's ServiceWorkerContainer object's ready promise.
    //    2. If readyPromise is null, then continue.
    //    3. If readyPromise is pending, resolve readyPromise with the result of getting the service worker registration
    //       object that represents registration in readyPromise's relevant settings object.
    // NB: The clients whose ready promises are pending have asked for the registration to resolve them with.
    m_ready_requests.remove_all_matching([&](ReadyRequest const& ready_request) {
        if (match_service_worker_registration(ready_request.partition, ready_request.creation_url) != &registration)
            return false;
        ready_request.connection.visit_open_connection([&](auto& client) {
            client.async_service_worker_ready(ready_request.request_id, snapshot_of(registration));
        });
        return true;
    });

    // 8. For each service worker client client who is using registration:
    auto active_worker_descriptor = *descriptor_of(active_worker.id);
    for (auto& client : m_controlled_clients) {
        if (!is_using(client, registration))
            continue;

        // 1. Set client's active worker to registration's active worker.
        client.active_service_worker = active_worker.id;

        // 2. Invoke Notify Controller Change algorithm with client as the argument.
        notify_controller_change(client.id, active_worker_descriptor);
    }
    if (previous_active_worker.has_value())
        forget_service_worker_if_unused(*previous_active_worker);

    // 9. Let activeWorker be registration's active worker.
    // 10. If the result of running the Should Skip Event algorithm with activeWorker and "activate" is false, then:
    if (!should_skip_event("activate"sv, active_worker.set_of_event_types_to_handle) && active_worker.process) {
        // 1. If the result of running the Run Service Worker algorithm with activeWorker is not failure, then:
        // 1. Queue a task task on activeWorker's event loop using the DOM manipulation task source to run the following
        //    steps:
        // 2. Wait for task to have executed or been discarded.
        // 3. Wait for the step labeled WaitForAsynchronousExtensions to complete.
        // NB: The worker's process runs the task, and reports to service_worker_did_finish_lifecycle_event() once the
        //     step labeled WaitForAsynchronousExtensions completes.
        dispatch_lifecycle_event(active_worker, ServiceWorkerLifecycleEvent::Activate);
        return;
    }

    finish_activate(active_worker);
}

// https://w3c.github.io/ServiceWorker/#activation-algorithm
void ServiceWorkerRegistry::finish_activate(ServiceWorkerRecord& worker)
{
    worker.awaited_step = AwaitedStep::None;

    // 11. Run the Update Worker State algorithm passing registration's active worker and "activated" as the arguments.
    update_worker_state(worker, ServiceWorkerState::Activated);
    stop_workers_of_partitions_without_clients();
}

void ServiceWorkerRegistry::dispatch_lifecycle_event(ServiceWorkerRecord& worker, ServiceWorkerLifecycleEvent event)
{
    worker.awaited_step = event == ServiceWorkerLifecycleEvent::Install ? AwaitedStep::InstallEvent : AwaitedStep::ActivateEvent;
    worker.process->async_dispatch_service_worker_lifecycle_event(event, lifecycle_event_deadline().to_milliseconds());

    // NB: The worker's process ends the event at its deadline. One that does not report the event's end soon after is
    //     terminated.
    auto worker_id = worker.id;
    worker.deadline_timer = Core::Timer::create_single_shot(lifecycle_event_deadline() + lifecycle_event_deadline_grace_period, [weak_this = make_weak_ptr(), worker_id] {
        auto* registry = weak_this.ptr();
        if (!registry)
            return;
        if (auto worker = registry->m_service_workers.get(worker_id); worker.has_value())
            registry->terminate_service_worker(**worker);
    });
    worker.deadline_timer->start();
}

void ServiceWorkerRegistry::service_worker_did_finish_lifecycle_event(Web::HTML::WorkerAgentId agent_id, ServiceWorkerLifecycleEvent event, bool succeeded)
{
    auto* worker = service_worker_of_agent(agent_id);
    if (!worker)
        return;
    if (event == ServiceWorkerLifecycleEvent::Install && worker->awaited_step == AwaitedStep::InstallEvent) {
        worker->deadline_timer->stop();
        finish_install(*worker, !succeeded);
    } else if (event == ServiceWorkerLifecycleEvent::Activate && worker->awaited_step == AwaitedStep::ActivateEvent) {
        worker->deadline_timer->stop();
        finish_activate(*worker);
    }
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-skipwaiting
void ServiceWorkerRegistry::service_worker_did_call_skip_waiting(Web::HTML::WorkerAgentId agent_id, ServiceWorkerRequestId request_id)
{
    auto* worker = service_worker_of_agent(agent_id);
    if (!worker)
        return;

    // 2. Run the following substeps in parallel:
    // 1. Set service worker's skip waiting flag.
    worker->skip_waiting_flag = true;

    // 2. Invoke Try Activate with service worker's containing service worker registration.
    // NB: When service worker is the active worker and a waiting worker replaces it, Try Activate terminates service
    //     worker and may forget its record, so worker is not used after this.
    if (auto* registration = registration_by_id(worker->registration_id))
        try_activate(*registration);

    // 3. Queue a task on service worker's responsible event loop, to resolve promise with undefined.
    // NB: The worker's process queues the task. A terminated worker has no agent, and its event loop runs no more
    //     tasks.
    if (auto* running_worker = service_worker_of_agent(agent_id))
        running_worker->process->async_service_worker_skip_waiting_finished(request_id);
}

void ServiceWorkerRegistry::service_worker_did_change_pending_events(Web::HTML::WorkerAgentId agent_id, bool has_pending_events)
{
    auto* worker = service_worker_of_agent(agent_id);
    if (!worker || worker->has_pending_events == has_pending_events)
        return;
    worker->has_pending_events = has_pending_events;
    if (has_pending_events)
        return;
    stop_workers_of_partitions_without_clients();

    // https://w3c.github.io/ServiceWorker/#extendableevent-add-lifetime-promise
    // 5.2. If event's pending promises count is 0, then:
    // 1. Let registration be the current global object's associated service worker's containing service worker
    //    registration.
    auto* registration = registration_by_id(worker->registration_id);
    if (!registration)
        return;

    // 2. If registration is unregistered, invoke Try Clear Registration with registration.
    if (is_unregistered(*registration)) {
        try_clear_registration(*registration);
        return;
    }

    // 3. If registration is not null, invoke Try Activate with registration.
    try_activate(*registration);
}

// https://w3c.github.io/ServiceWorker/#service-worker-postmessage-options
void ServiceWorkerRegistry::post_message_to_service_worker(ServiceWorkerClientEnvironment const& client, ServiceWorkerClientSnapshot source, ServiceWorkerId service_worker_id, Web::HTML::SerializedTransferRecord message)
{
    auto partition = partition_of(client.settings, client.has_cross_site_ancestor);
    if (!partition.has_value())
        return;

    // AD-HOC: The source's id is that of the canonical record of the environment that sent the message.
    source.id = client.settings.id();

    // 2. Let incumbentSettings be the incumbent settings object.
    // 6.2.2. Let origin be incumbentSettings's origin.
    dispatch_message_event(*partition, service_worker_id, move(source), client.settings.origin(), move(message));
}

void ServiceWorkerRegistry::service_worker_did_post_message_to_service_worker(Web::HTML::WorkerAgentId agent_id, ServiceWorkerId service_worker_id, Web::HTML::SerializedTransferRecord message)
{
    auto* sender = service_worker_of_agent(agent_id);
    if (!sender)
        return;

    // 6.2.1. Let source be determined by switching on the type of incumbentGlobal:
    //        ServiceWorkerGlobalScope
    //            The result of getting the service worker object that represents incumbentGlobal's service worker in
    //            the relevant settings object of serviceWorker's global object.
    // NB: The service worker's process gets the object from the descriptor.
    auto source = ServiceWorkerDescriptor { .id = sender->id, .script_url = sender->script_url, .state = sender->state };
    auto origin = sender->partition.storage_key.origin;
    dispatch_message_event(sender->partition, service_worker_id, move(source), origin, move(message));
}

// https://w3c.github.io/ServiceWorker/#service-worker-postmessage-options
void ServiceWorkerRegistry::dispatch_message_event(Partition const& sender_partition, ServiceWorkerId service_worker_id, ServiceWorkerMessageSource source, URL::Origin const& origin, Web::HTML::SerializedTransferRecord message)
{
    // 1. Let serviceWorker be the service worker represented by this.
    auto worker = m_service_workers.get(service_worker_id);
    if (!worker.has_value())
        return;
    auto& service_worker = **worker;

    // AD-HOC: Only an environment in the partition of the worker's registration can have an object for the worker.
    if (service_worker.partition != sender_partition)
        return;

    // 5. If the result of running the Should Skip Event algorithm with "message" and serviceWorker is true, then return.
    if (should_skip_event("message"sv, service_worker.set_of_event_types_to_handle))
        return;

    // 6. Run these substeps in parallel:
    //    1. If the result of running the Run Service Worker algorithm with serviceWorker is failure, then return.
    if (!run_service_worker(service_worker))
        return;
    if (service_worker.awaited_step == AwaitedStep::Restart) {
        service_worker.messages_waiting_for_run.append({ move(source), origin, move(message) });
        return;
    }

    //    2. Queue a task on the DOM manipulation task source to run the following steps:
    // NB: The worker's process queues the task, and ends the event at its deadline. The heartbeat stops a worker whose
    //     event loop no longer runs, and so cannot end the event.
    service_worker.process->async_dispatch_service_worker_message_event(move(source), origin, move(message), lifecycle_event_deadline().to_milliseconds());
}

// https://w3c.github.io/ServiceWorker/#client-postmessage-options
void ServiceWorkerRegistry::service_worker_did_post_message_to_client(Web::HTML::WorkerAgentId agent_id, String const& client_id, Web::HTML::SerializedTransferRecord message)
{
    auto* service_worker = service_worker_of_agent(agent_id);
    if (!service_worker)
        return;

    // 4.1. Let targetClient be null.
    Optional<ServiceWorkerClientConnection> target_connection;
    Optional<Web::HTML::EnvironmentId> target_client;

    // 4.2. For each service worker client client:
    //      1. If client is the contextObject's service worker client, set targetClient to client, and break.
    // AD-HOC: Only a client in the partition of the worker's registration can be one the worker has a Client object for.
    WebContentClient::for_each_client([&](WebContentClient& client) {
        if (client.is_private() != m_is_private)
            return IterationDecision::Continue;
        client.for_each_page([&](WebContentPage& page) {
            page.for_each_hosted_environment([&](CanonicalEnvironmentSettingsObject const& settings, bool has_cross_site_ancestor) {
                if (settings.id().to_string() != client_id || partition_of(settings, has_cross_site_ancestor) != service_worker->partition)
                    return IterationDecision::Continue;
                target_connection = ServiceWorkerClientConnection { client };
                target_client = settings.id();
                return IterationDecision::Break;
            });
            return target_client.has_value() ? IterationDecision::Break : IterationDecision::Continue;
        });
        return target_client.has_value() ? IterationDecision::Break : IterationDecision::Continue;
    });
    if (!target_client.has_value()) {
        WorkerAgentManager::the().for_each_agent_environment([&](WebWorkerClient& client, CanonicalWorkerEnvironmentSettingsObject const& settings, IsPrivate is_private, Web::HTML::AgentType agent_type) {
            if (target_client.has_value() || agent_type == Web::HTML::AgentType::ServiceWorker || is_private != m_is_private)
                return;
            if (settings.id().to_string() != client_id || partition_of(settings, settings.has_cross_site_ancestor()) != service_worker->partition)
                return;
            target_connection = ServiceWorkerClientConnection { client };
            target_client = settings.id();
        });
    }

    // 4.3. If targetClient is null, return.
    if (!target_client.has_value())
        return;

    // 4.4. Let destination be the ServiceWorkerContainer object whose associated service worker client is targetClient.
    // 4.5. Add a task that runs the following steps to destination's client message queue:
    // NB: The target client's process adds the task.
    auto source = ServiceWorkerDescriptor { .id = service_worker->id, .script_url = service_worker->script_url, .state = service_worker->state };
    target_connection->visit_open_connection([&](auto& client) {
        client.async_service_worker_message_to_client(*target_client, source, service_worker->partition.storage_key.origin, move(message));
    });
}

// https://w3c.github.io/ServiceWorker/#run-service-worker-algorithm
// NB: Returns whether the worker runs, or starts to.
bool ServiceWorkerRegistry::run_service_worker(ServiceWorkerRecord& worker)
{
    // 1. If serviceWorker is running, then return serviceWorker's start status.
    if (worker.process)
        return worker.process->is_open();

    // 2. If serviceWorker's state is "redundant", then return failure.
    if (worker.state == ServiceWorkerState::Redundant)
        return false;

    // 3. Assert: serviceWorker's start status is null.
    // 4. Let script be serviceWorker's script resource.
    // 5. Assert: script is not null.
    // NB: Only a worker that installed has the scripts that its process recorded.
    auto* registration = registration_by_id(worker.registration_id);
    if (worker.script_resources.is_empty() || !registration)
        return false;

    // 6. Let startFailed be false.
    // 7. Let workerGlobalScope be serviceWorker's global object.
    // 8. If workerGlobalScope is null:
    //    1. Set workerGlobalScope to be the result of running the Setup ServiceWorkerGlobalScope algorithm with
    //       serviceWorker.
    // NB: A new process sets the worker's global object up, and runs the worker from its scripts.
    auto& client = worker.registering_client;
    auto agent = WorkerAgentManager::the().start_service_worker_agent(m_is_private, client.origin, client.top_level_origin, client.has_cross_site_ancestor);
    if (agent.is_error()) {
        dbgln("Unable to start a service worker process: {}", agent.error());
        return false;
    }
    worker.agent_id = agent.value().id;
    worker.process = agent.value().client;
    worker.awaited_step = AwaitedStep::Restart;
    worker.process->async_start_service_worker(ServiceWorkerStartRequest {
        .worker = ServiceWorkerDescriptor { .id = worker.id, .script_url = worker.script_url, .state = worker.state },
        .worker_type = worker.type,
        .registration = snapshot_of(*registration),
        .environment_id = agent.value().environment_id,
        .origin = client.origin,
        .has_cross_site_ancestor = client.has_cross_site_ancestor,
        .top_level_origin = client.top_level_origin,
        .script_fetch = ServiceWorkerScriptFetchParameters { .job_client = client, .force_bypass_cache = false },
        .stored_scripts = worker.script_resources,
        .extendable_event_timeout_ms = static_cast<u64>(lifecycle_event_deadline().to_milliseconds()),
    });

    // 10. Wait for serviceWorker to be running, or for startFailed to be true.
    // NB: The worker's process reports whether the worker runs, to service_worker_did_run().
    auto worker_id = worker.id;
    worker.deadline_timer = Core::Timer::create_single_shot(installed_service_worker_start_deadline, [weak_this = make_weak_ptr(), worker_id] {
        auto* registry = weak_this.ptr();
        if (!registry)
            return;
        if (auto worker = registry->m_service_workers.get(worker_id); worker.has_value())
            registry->terminate_service_worker(**worker);
    });
    worker.deadline_timer->start();
    start_heartbeat(worker);
    return true;
}

void ServiceWorkerRegistry::service_worker_did_record_scripts(Web::HTML::WorkerAgentId agent_id, Vector<ServiceWorkerScriptResource> scripts)
{
    auto* worker = service_worker_of_agent(agent_id);
    if (!worker || !first_is_one_of(worker->awaited_step, AwaitedStep::Start, AwaitedStep::InstallEvent))
        return;
    worker->script_resources = move(scripts);
}

void ServiceWorkerRegistry::start_heartbeat(ServiceWorkerRecord& worker)
{
    auto worker_id = worker.id;
    worker.heartbeat_timer = Core::Timer::create_repeating(heartbeat_interval(), [weak_this = make_weak_ptr(), worker_id] {
        auto* registry = weak_this.ptr();
        if (!registry)
            return;
        if (auto worker = registry->m_service_workers.get(worker_id); worker.has_value())
            registry->send_heartbeat(**worker);
    });
    worker.heartbeat_timer->start();
}

void ServiceWorkerRegistry::send_heartbeat(ServiceWorkerRecord& worker)
{
    if (!worker.process)
        return;
    if (worker.awaited_step == AwaitedStep::None && !worker.has_pending_events) {
        worker.unanswered_heartbeat = {};
        return;
    }

    // NB: A worker whose event loop did not answer the last heartbeat is stuck, which fails what it was doing.
    if (worker.unanswered_heartbeat.has_value()) {
        dbgln("Service worker {} did not answer a heartbeat, stopping it", worker.script_url);
        terminate_service_worker(worker);
        return;
    }
    worker.unanswered_heartbeat = ++m_next_heartbeat_id;
    worker.process->async_service_worker_heartbeat(*worker.unanswered_heartbeat);
}

void ServiceWorkerRegistry::service_worker_did_respond_to_heartbeat(Web::HTML::WorkerAgentId agent_id, u64 heartbeat_id)
{
    auto* worker = service_worker_of_agent(agent_id);
    if (worker && worker->unanswered_heartbeat == heartbeat_id)
        worker->unanswered_heartbeat = {};
}

void ServiceWorkerRegistry::terminate_service_worker_for_testing(ServiceWorkerId service_worker_id, bool kill_process)
{
    auto worker = m_service_workers.get(service_worker_id);
    if (!worker.has_value() || !(*worker)->process)
        return;
    if (!kill_process) {
        terminate_service_worker(**worker);
        return;
    }
    // NB: The worker stops as it does when its process crashes, once its connection closes. A service worker has a
    //     process of its own, which nothing else may share for this to stand in for a crash of the worker alone.
    auto& process = *(*worker)->process;
    if (!WorkerAgentManager::the().runs_alone_in_its_host(process)) {
        dbgln("Not killing the process of a service worker that shares it");
        return;
    }
    if (auto result = Core::System::kill(process.host()->pid(), SIGKILL); result.is_error())
        dbgln("Unable to kill a service worker process: {}", result.error());
}

bool ServiceWorkerRegistry::has_client_in_partition(Partition const& partition) const
{
    bool has_client = false;
    WebContentClient::for_each_client([&](WebContentClient& client) {
        if (client.is_private() != m_is_private)
            return IterationDecision::Continue;
        client.for_each_page([&](WebContentPage& page) {
            page.for_each_hosted_environment([&](CanonicalEnvironmentSettingsObject const& settings, bool has_cross_site_ancestor) {
                has_client = partition_of(settings, has_cross_site_ancestor) == partition;
                return has_client ? IterationDecision::Break : IterationDecision::Continue;
            });
            return has_client ? IterationDecision::Break : IterationDecision::Continue;
        });
        return has_client ? IterationDecision::Break : IterationDecision::Continue;
    });
    if (has_client)
        return true;
    WorkerAgentManager::the().for_each_agent_environment([&](WebWorkerClient&, CanonicalWorkerEnvironmentSettingsObject const& settings, IsPrivate is_private, Web::HTML::AgentType agent_type) {
        if (agent_type != Web::HTML::AgentType::ServiceWorker && is_private == m_is_private && partition_of(settings, settings.has_cross_site_ancestor()) == partition)
            has_client = true;
    });
    return has_client;
}

// AD-HOC: A running worker keeps running while a client in its partition exists, and stops a while after the last one
//         goes away, as WebKit's workers do. A worker that has pending events keeps running until they end.
void ServiceWorkerRegistry::stop_workers_of_partitions_without_clients()
{
    Vector<Partition> partitions_with_running_workers;
    for (auto const& [id, worker] : m_service_workers) {
        if (worker->process && !partitions_with_running_workers.contains_slow(worker->partition))
            partitions_with_running_workers.append(worker->partition);
    }

    for (auto const& partition : partitions_with_running_workers) {
        auto termination = m_partition_terminations.find_if([&](auto const& termination) { return termination.partition == partition; });
        if (has_client_in_partition(partition)) {
            if (termination != m_partition_terminations.end())
                m_partition_terminations.remove(termination.index());
            continue;
        }
        if (termination != m_partition_terminations.end())
            continue;
        auto timer = Core::Timer::create_single_shot(termination_delay(), [weak_this = make_weak_ptr(), partition] {
            if (auto* registry = weak_this.ptr())
                registry->stop_idle_workers_of_partition(partition);
        });
        timer->start();
        m_partition_terminations.append({ partition, move(timer) });
    }
}

void ServiceWorkerRegistry::stop_idle_workers_of_partition(Partition const& partition)
{
    // NB: The timer that calls this outlives its own handler.
    m_partition_terminations.remove_all_matching([&](auto& termination) {
        if (termination.partition != partition)
            return false;
        Core::deferred_invoke([timer = termination.timer] { });
        return true;
    });
    if (has_client_in_partition(partition))
        return;

    Vector<ServiceWorkerId> idle_workers;
    bool has_busy_worker = false;
    for (auto const& [id, worker] : m_service_workers) {
        if (worker->partition != partition || !worker->process)
            continue;
        if (worker->awaited_step == AwaitedStep::None && !worker->has_pending_events)
            idle_workers.append(id);
        else
            has_busy_worker = true;
    }
    for (auto id : idle_workers) {
        if (auto worker = m_service_workers.get(id); worker.has_value())
            terminate_service_worker(**worker);
    }

    // NB: A worker that was busy stops once it is idle, which the next timer checks.
    if (has_busy_worker)
        stop_workers_of_partitions_without_clients();
}

// https://w3c.github.io/ServiceWorker/#navigation-preload-manager
void ServiceWorkerRegistry::navigation_preload(ServiceWorkerClientEnvironment const& client, ServiceWorkerRequestId request_id, ServiceWorkerRegistrationId registration_id, ServiceWorkerNavigationPreloadOperation operation, ByteString header_value)
{
    // 1. Let registration be this's associated service worker registration.
    // AD-HOC: Only an environment in the partition of the registration can have an object for it.
    auto* registration = registration_by_id(registration_id);
    if (registration && partition_of(client.settings, client.has_cross_site_ancestor) != registration->key.partition)
        registration = nullptr;

    Optional<ServiceWorkerNavigationPreloadState> state;
    if (registration && operation == ServiceWorkerNavigationPreloadOperation::GetState) {
        // https://w3c.github.io/ServiceWorker/#navigation-preload-manager-getstate
        // 2.2. Let state be a new NavigationPreloadState dictionary.
        // 2.3. If registration's navigation preload enabled flag is set, set state["enabled"] to true.
        // 2.4. Set state["headerValue"] to registration's navigation preload header value.
        state = ServiceWorkerNavigationPreloadState { .enabled = registration->navigation_preload_enabled, .header_value = registration->navigation_preload_header_value };
    } else if (registration && registration->active_worker.has_value()) {
        // https://w3c.github.io/ServiceWorker/#navigation-preload-manager-enable
        // 2.2. If registration's active worker is null, queue a task on promise's relevant settings object's
        //      responsible event loop, using the DOM manipulation task source, to reject promise with an
        //      "InvalidStateError" DOMException, and abort these steps.
        switch (operation) {
        case ServiceWorkerNavigationPreloadOperation::Enable:
            // 2.3. Set registration's navigation preload enabled flag.
            registration->navigation_preload_enabled = true;
            break;
        case ServiceWorkerNavigationPreloadOperation::Disable:
            // https://w3c.github.io/ServiceWorker/#navigation-preload-manager-disable
            // 2.3. Unset registration's navigation preload enabled flag.
            registration->navigation_preload_enabled = false;
            break;
        case ServiceWorkerNavigationPreloadOperation::SetHeaderValue:
            // https://w3c.github.io/ServiceWorker/#navigation-preload-manager-setheadervalue
            // 4.3. Set registration's navigation preload header value to value.
            registration->navigation_preload_header_value = move(header_value);
            break;
        case ServiceWorkerNavigationPreloadOperation::GetState:
            VERIFY_NOT_REACHED();
        }
        state = ServiceWorkerNavigationPreloadState {};
    }

    // NB: The client's process resolves the promise with the state, or rejects it when there is none.
    client.connection.visit_open_connection([&](auto& connection) {
        connection.async_navigation_preload_settled(request_id, state);
    });
}

// https://w3c.github.io/ServiceWorker/#clients-matchall
// https://w3c.github.io/ServiceWorker/#create-fetch-event-and-dispatch
void ServiceWorkerRegistry::connect_fetch_channel(ServiceWorkerClientConnection const& requester, ServiceWorkerRequestId request_id, Web::HTML::EnvironmentId const& client_id, ServiceWorkerId worker_id)
{
    auto reply = [&](Optional<IPC::TransportHandle> handle) {
        requester.visit_open_connection([&](auto& client) { client.async_service_worker_fetch_channel_ready(request_id, move(handle)); });
    };

    // AD-HOC: A channel carries the fetch events of one client whose active service worker the worker is, from the
    //         process that hosts the client, or that reserved it for a navigation's fetch. A client's controller may
    //         have changed since it asked.
    auto client = m_controlled_clients.find_if([&](ControlledClient const& client) { return client.id == client_id; });
    if (client == m_controlled_clients.end() || client->active_service_worker != worker_id) {
        reply({});
        return;
    }
    bool requester_has_client = client->reserving_process && client->reserving_process == requester.identity();
    if (!requester_has_client && !client->is_reserved()) {
        for_each_hosted_client([&](HostedClient const& hosted_client) {
            requester_has_client = hosted_client.settings.id() == client_id && hosted_client.connection.identity() == requester.identity();
            return requester_has_client ? IterationDecision::Break : IterationDecision::Continue;
        });
    }
    if (!requester_has_client) {
        reply({});
        return;
    }

    // 16. If the result of running the Run Service Worker algorithm with activeWorker is failure, then set
    //     handleFetchFailed to true.
    auto worker = m_service_workers.get(worker_id);
    if (!worker.has_value() || !run_service_worker(**worker)) {
        reply({});
        return;
    }

    auto channel = IPC::TransportHandle::create_pair();
    if (channel.is_error()) {
        dbgln("Unable to create a service worker fetch channel: {}", channel.error());
        reply({});
        return;
    }
    (*worker)->process->async_connect_service_worker_fetch_channel(move(channel.value().first), client_id);
    reply(move(channel.value().second));
}

// https://w3c.github.io/ServiceWorker/#clients-claim
void ServiceWorkerRegistry::service_worker_did_request_claim(Web::HTML::WorkerAgentId agent_id, ServiceWorkerRequestId request_id)
{
    auto* worker = service_worker_of_agent(agent_id);
    if (!worker)
        return;
    auto worker_id = worker->id;
    auto* registration = registration_by_id(worker->registration_id);

    // 1. If the service worker is not an active worker, return a promise rejected with an "InvalidStateError"
    //    DOMException.
    // NB: The worker's process checks this before it asks, and the worker may have stopped being one since.
    if (!registration || registration->active_worker != worker_id) {
        worker->process->async_service_worker_claim_finished(request_id, false);
        return;
    }

    // 3. Run the following substeps in parallel:
    //    1. For each service worker client client where the result of running obtain a storage key given client equals
    //       the service worker's containing service worker registration's storage key:
    // AD-HOC: Only the clients in the partition of the worker's registration are clients of the worker.
    Vector<Web::HTML::EnvironmentId> clients_to_claim;
    for_each_hosted_client([&](HostedClient const& client) {
        if (partition_of(client.settings, client.has_cross_site_ancestor) != worker->partition)
            return IterationDecision::Continue;

        // 1. If client's execution ready flag is unset or client's discarded flag is set, continue.
        // NB: A client has a canonical record once it is execution ready, and until it is discarded.

        // 2. If client is not a secure context, continue.
        // FIXME: Check whether the client is a secure context, rather than whether its origin is potentially trustworthy.
        if (Web::SecureContexts::is_origin_potentially_trustworthy(client.settings.origin()) != Web::SecureContexts::Trustworthiness::PotentiallyTrustworthy)
            return IterationDecision::Continue;

        // 3. Let storage key be the result of running obtain a storage key given client.
        // 4. Let registration be the result of running Match Service Worker Registration given storage key and client's
        //    creation URL.
        auto creation_url = client.settings.creation_url();
        if (!creation_url.has_value())
            return IterationDecision::Continue;
        auto* matched_registration = match_service_worker_registration(worker->partition, *creation_url);

        // 5. If registration is not the service worker's containing service worker registration, continue.
        // NOTE: registration will be null if the service worker's containing service worker registration is
        //       unregistered.
        if (matched_registration != registration)
            return IterationDecision::Continue;

        // 6. If client's active service worker is not the service worker, then:
        auto controlled_client = m_controlled_clients.find_if([&](ControlledClient const& controlled_client) { return controlled_client.id == client.settings.id(); });
        if (controlled_client == m_controlled_clients.end() || controlled_client->active_service_worker != worker_id)
            clients_to_claim.append(client.settings.id());
        return IterationDecision::Continue;
    });

    auto descriptor = *descriptor_of(worker_id);
    for (auto const& client : clients_to_claim) {
        // 1. Invoke Handle Service Worker Client Unload with client as the argument.
        handle_service_worker_client_unload(client);

        // 2. Set client's active service worker to service worker.
        m_controlled_clients.append(ControlledClient {
            .id = client,
            .active_service_worker = worker_id,
            .reserving_navigable = {},
            .reserving_navigation = nullptr,
            .reserving_process = nullptr,
        });

        // 3. Invoke Notify Controller Change algorithm with client as the argument.
        notify_controller_change(client, descriptor);
    }

    //    2. Queue a task on promise's relevant settings object's responsible event loop using the DOM manipulation task
    //       source to resolve promise with undefined.
    // NB: The worker's process queues the task.
    if (auto worker_record = m_service_workers.get(worker_id); worker_record.has_value() && (*worker_record)->process)
        (*worker_record)->process->async_service_worker_claim_finished(request_id, true);
}

void ServiceWorkerRegistry::service_worker_did_request_clients(Web::HTML::WorkerAgentId agent_id, ServiceWorkerRequestId request_id, String const& client_id, bool is_get, bool include_uncontrolled, Web::Bindings::ClientType type)
{
    auto* worker = service_worker_of_agent(agent_id);
    if (!worker)
        return;
    using Web::Bindings::ClientType;

    ClientsRequest request {
        .gather_id = ++m_next_gather_id,
        .worker_id = worker->id,
        .request_id = request_id,
        .is_get = is_get,
        .window_clients = {},
        .worker_clients = {},
        .processes_waited_for = {},
        .snapshots = {},
        .timeout = {},
    };

    // 2.2. For each service worker client client where the result of running obtain a storage key given client equals
    //      the associated service worker's containing service worker registration's storage key:
    // AD-HOC: Only the clients in the partition of the worker's registration are clients of the worker.
    //     3. If options["includeUncontrolled"] is false, and if client's active service worker is not the associated
    //        service worker, continue.
    // NB: get() matches a client of the id, controlled or not.
    auto matches = [&](Web::HTML::EnvironmentId const& id, bool is_window, Optional<Web::HTML::AgentType> agent_type) {
        if (is_get)
            return id.to_string() == client_id;
        if (!include_uncontrolled && !any_of(m_controlled_clients, [&](ControlledClient const& client) { return client.id == id && client.active_service_worker == worker->id; }))
            return false;
        // 2.5.1. If options["type"] is "window" or "all", and client is not an environment settings object or is a
        //        window client, then:
        if (is_window)
            return first_is_one_of(type, ClientType::Window, ClientType::All);
        // 2.5.2. Else if options["type"] is "worker" or "all" and client is a dedicated worker client, or
        //        options["type"] is "sharedworker" or "all" and client is a shared worker client, then:
        if (agent_type == Web::HTML::AgentType::DedicatedWorker)
            return first_is_one_of(type, ClientType::Worker, ClientType::All);
        return agent_type == Web::HTML::AgentType::SharedWorker && first_is_one_of(type, ClientType::Sharedworker, ClientType::All);
    };

    WebContentClient::for_each_client([&](WebContentClient& client) {
        if (client.is_private() != m_is_private)
            return IterationDecision::Continue;
        client.for_each_page([&](WebContentPage& page) {
            Vector<Web::HTML::EnvironmentId> ids;
            page.for_each_hosted_environment([&](CanonicalEnvironmentSettingsObject const& settings, bool has_cross_site_ancestor) {
                if (partition_of(settings, has_cross_site_ancestor) == worker->partition && matches(settings.id(), true, {}))
                    ids.append(settings.id());
                return IterationDecision::Continue;
            });
            if (ids.is_empty())
                return IterationDecision::Continue;
            request.window_clients.extend(ids);
            request.processes_waited_for.append(ServiceWorkerClientConnection { client }.identity());
            client.async_request_service_worker_client_snapshots(page.id(), request.gather_id, move(ids));
            return IterationDecision::Continue;
        });
        return IterationDecision::Continue;
    });
    WorkerAgentManager::the().for_each_agent_environment([&](WebWorkerClient& client, CanonicalWorkerEnvironmentSettingsObject const& settings, IsPrivate is_private, Web::HTML::AgentType agent_type) {
        if (agent_type == Web::HTML::AgentType::ServiceWorker || is_private != m_is_private)
            return;
        if (partition_of(settings, settings.has_cross_site_ancestor()) != worker->partition || !matches(settings.id(), false, agent_type))
            return;
        request.worker_clients.append(settings.id());
        request.processes_waited_for.append(ServiceWorkerClientConnection { client }.identity());
        client.async_request_service_worker_client_snapshots(request.gather_id, Vector { settings.id() });
    });

    auto gather_id = request.gather_id;
    bool waits_for_processes = !request.processes_waited_for.is_empty();
    // NB: A process that never describes its clients does not keep the worker waiting.
    request.timeout = Core::Timer::create_single_shot(AK::Duration::from_seconds(5), [weak_this = make_weak_ptr(), gather_id] {
        if (auto* registry = weak_this.ptr())
            registry->finish_clients_request(gather_id);
    });
    request.timeout->start();
    m_clients_requests.append(move(request));
    if (!waits_for_processes)
        finish_clients_request(gather_id);
}

void ServiceWorkerRegistry::client_did_provide_service_worker_client_snapshots(u64 gather_id, ServiceWorkerClientConnection const& provider, Vector<ServiceWorkerClientSnapshot> snapshots)
{
    auto request = m_clients_requests.find_if([&](auto const& request) { return request.gather_id == gather_id; });
    if (request == m_clients_requests.end())
        return;
    auto waited_for = request->processes_waited_for.find_first_index(provider.identity());
    if (!waited_for.has_value())
        return;
    request->processes_waited_for.remove(*waited_for);

    // NB: A process describes only the clients it was asked about.
    for (auto& snapshot : snapshots) {
        if (request->window_clients.contains_slow(snapshot.id) || request->worker_clients.contains_slow(snapshot.id))
            request->snapshots.append(move(snapshot));
    }
    if (request->processes_waited_for.is_empty())
        finish_clients_request(gather_id);
}

void ServiceWorkerRegistry::finish_clients_request(u64 gather_id)
{
    auto index = m_clients_requests.find_first_index_if([&](auto const& request) { return request.gather_id == gather_id; });
    if (!index.has_value())
        return;
    auto request = m_clients_requests.take(*index);
    if (request.timeout) {
        request.timeout->stop();
        // NB: The timeout may be what finishes the request, so it outlives its own handler.
        Core::deferred_invoke([timer = move(request.timeout)] { });
    }

    // 2.6.4. Sort clientObjects such that:
    //        - WindowClient objects whose browsing context has been focused are placed first, sorted in the most
    //          recently focused order.
    //        - WindowClient objects whose browsing context has never been focused are placed next, sorted in their
    //          service worker client's creation order.
    //        - Client objects whose associated service worker client is a worker client are placed next, sorted in
    //          their service worker client's creation order.
    // FIXME: Sort the window clients that have been focused by when they were, and the others by when they were
    //        created. A focused one comes first, and the rest come in the order the UI process finds them.
    Vector<ServiceWorkerClientSnapshot> clients;
    for (auto pass : { 0, 1, 2 }) {
        for (auto& snapshot : request.snapshots) {
            auto group = snapshot.type != Web::Bindings::ClientType::Window ? 2 : (snapshot.focused ? 0 : 1);
            if (group == pass)
                clients.append(move(snapshot));
        }
    }
    if (request.is_get && clients.size() > 1)
        clients.shrink(1);

    auto worker = m_service_workers.get(request.worker_id);
    if (!worker.has_value() || !(*worker)->process)
        return;
    (*worker)->process->async_service_worker_clients_matched(request.request_id, clients);
}

// https://w3c.github.io/ServiceWorker/#terminate-service-worker
void ServiceWorkerRegistry::terminate_service_worker(ServiceWorkerRecord& worker)
{
    // 1. Run the following steps in parallel with serviceWorker's main loop:
    // NB: The worker's process runs the steps as it closes.
    if (worker.agent_id.has_value())
        WorkerAgentManager::the().close_service_worker_agent(*worker.agent_id);
    service_worker_stopped(worker);
}

void ServiceWorkerRegistry::service_worker_process_did_exit(Web::HTML::WorkerAgentId agent_id)
{
    if (auto* worker = service_worker_of_agent(agent_id))
        service_worker_stopped(*worker);
}

void ServiceWorkerRegistry::service_worker_stopped(ServiceWorkerRecord& worker)
{
    worker.agent_id = {};
    worker.process = nullptr;
    worker.has_pending_events = false;
    worker.unanswered_heartbeat = {};
    worker.messages_waiting_for_run.clear();

    // NB: The deadline and heartbeat timers may be what stops the worker, so they outlive their own handlers.
    for (auto* timer : { &worker.deadline_timer, &worker.heartbeat_timer }) {
        if (!*timer)
            continue;
        (*timer)->stop();
        Core::deferred_invoke([timer = move(*timer)] { });
    }

    switch (worker.awaited_step) {
    case AwaitedStep::None:
        break;
    case AwaitedStep::Start:
        fail_update(worker, ServiceWorkerJobError::TypeError);
        break;
    case AwaitedStep::Restart:
        worker.awaited_step = AwaitedStep::None;
        break;
    case AwaitedStep::InstallEvent:
        finish_install(worker, true);
        break;
    case AwaitedStep::ActivateEvent:
        finish_activate(worker);
        break;
    }
}

void ServiceWorkerRegistry::forget_service_worker_if_unused(ServiceWorkerId worker_id)
{
    auto worker = m_service_workers.get(worker_id);
    if (!worker.has_value() || (*worker)->process || (*worker)->awaited_step != AwaitedStep::None)
        return;
    auto* registration = registration_by_id((*worker)->registration_id);
    if (registration && first_is_one_of(Optional<ServiceWorkerId> { worker_id }, registration->installing_worker, registration->waiting_worker, registration->active_worker))
        return;

    // NB: A redundant worker stays the active service worker of the clients it controls, until they unload.
    if (any_of(m_controlled_clients, [&](ControlledClient const& client) { return client.active_service_worker == worker_id; }))
        return;
    m_service_workers.remove(worker_id);
}

// https://w3c.github.io/ServiceWorker/#unregister-algorithm
void ServiceWorkerRegistry::unregister(RegistrationKey const& job_queue_key, Job& job)
{
    // 1. Let registration be the result of running Get Registration given job's storage key and job's scope url.
    auto* registration = get_registration(job.partition, job.scope_url);

    // 2. If registration is null, then:
    if (!registration) {
        // 1. Invoke Resolve Job Promise with job and false.
        resolve_job_promise(job, false);

        // 2. Invoke Finish Job with job and abort these steps.
        finish_job(job_queue_key, job);
        return;
    }

    // 3. Remove registration map[(registration's storage key, job's scope url)].
    m_unregistered_registrations.append(m_registration_map.take(registration->key).release_value());
    auto& unregistered_registration = *m_unregistered_registrations.last();

    // 4. Invoke Resolve Job Promise with job and true.
    resolve_job_promise(job, true);

    // 5. Invoke Try Clear Registration with registration.
    // NOTE: If Try Clear Registration does not trigger Clear Registration here, Clear Registration is tried again when
    //       the last client using the registration is unloaded or the extend lifetime promises for the registration's
    //       service workers settle.
    try_clear_registration(unregistered_registration);

    // 6. Invoke Finish Job with job.
    finish_job(job_queue_key, job);
}

// https://w3c.github.io/ServiceWorker/#try-clear-registration-algorithm
void ServiceWorkerRegistry::try_clear_registration(Registration& registration)
{
    // 1. Invoke Clear Registration with registration if no service worker client is using registration and all of the
    //    following conditions are true:
    //    - registration's installing worker is null or the result of running Service Worker Has No Pending Events with
    //      registration's installing worker is true.
    //    - registration's waiting worker is null or the result of running Service Worker Has No Pending Events with
    //      registration's waiting worker is true.
    //    - registration's active worker is null or the result of running Service Worker Has No Pending Events with
    //      registration's active worker is true.
    if (any_service_worker_client_is_using(registration)
        || !service_worker_has_no_pending_events(registration.installing_worker)
        || !service_worker_has_no_pending_events(registration.waiting_worker)
        || !service_worker_has_no_pending_events(registration.active_worker))
        return;
    clear_registration(registration);

    // NB: Nothing refers to a cleared registration that was unregistered.
    m_unregistered_registrations.remove_first_matching([&](auto const& unregistered_registration) {
        return unregistered_registration.ptr() == &registration;
    });
}

// https://w3c.github.io/ServiceWorker/#clear-registration-algorithm
void ServiceWorkerRegistry::clear_registration(Registration& registration)
{
    // NB: A job that waits for one of the registration's workers, which only the test-web reset clears during a job,
    //     finishes once the registration is cleared. Terminating the worker does not finish the step it awaited.
    Optional<JobInProgress> interrupted_job;

    // 1. Run the following steps atomically.
    auto clear_slot = [&](ServiceWorkerRegistrationSlot slot, Optional<ServiceWorkerId> worker_id) {
        auto& worker = **m_service_workers.get(*worker_id);
        if (worker.job_in_progress.has_value())
            interrupted_job = worker.job_in_progress.release_value();
        worker.awaited_step = AwaitedStep::None;

        // 1. Terminate registration's installing worker.
        terminate_service_worker(worker);

        // 2. Run the Update Worker State algorithm passing registration's installing worker and "redundant" as the
        //    arguments.
        update_worker_state(worker, ServiceWorkerState::Redundant);

        // 3. Run the Update Registration State algorithm passing registration, "installing" and null as the arguments.
        update_registration_state(registration, slot, {});
        forget_service_worker_if_unused(worker.id);
    };

    // 2. If registration's installing worker is not null, then:
    if (registration.installing_worker.has_value())
        clear_slot(ServiceWorkerRegistrationSlot::Installing, registration.installing_worker);

    // 3. If registration's waiting worker is not null, then:
    // NB: Its steps are those of step 2, for the waiting worker.
    if (registration.waiting_worker.has_value())
        clear_slot(ServiceWorkerRegistrationSlot::Waiting, registration.waiting_worker);

    // 4. If registration's active worker is not null, then:
    // NB: Its steps are those of step 2, for the active worker.
    if (registration.active_worker.has_value())
        clear_slot(ServiceWorkerRegistrationSlot::Active, registration.active_worker);

    if (interrupted_job.has_value()) {
        auto job_queue = m_scope_to_job_queue_map.get(interrupted_job->job_queue_key);
        finish_job(interrupted_job->job_queue_key, *job_queue->first());
    }
}

// https://w3c.github.io/ServiceWorker/#update-registration-state-algorithm
void ServiceWorkerRegistry::update_registration_state(Registration& registration, ServiceWorkerRegistrationSlot target, Optional<ServiceWorkerId> source)
{
    // 1. Let registrationObjects be an array containing all the ServiceWorkerRegistration objects associated with
    //    registration.
    // NB: The processes that host environments in the registration's partition find their own objects of the
    //     registration, and queue the tasks of the steps below.

    switch (target) {
    // 2. If target is "installing", then:
    case ServiceWorkerRegistrationSlot::Installing:
        // 1. Set registration's installing worker to source.
        registration.installing_worker = source;

        // 2. For each registrationObject in registrationObjects:
        //    1. Queue a task to set the installing attribute of registrationObject to null if registration's installing
        //       worker is null, or the result of getting the service worker object that represents registration's
        //       installing worker in registrationObject's relevant settings object.
        break;

    // 3. Else if target is "waiting", then:
    case ServiceWorkerRegistrationSlot::Waiting:
        // 1. Set registration's waiting worker to source.
        registration.waiting_worker = source;

        // 2. For each registrationObject in registrationObjects:
        //    1. Queue a task to set the waiting attribute of registrationObject to null if registration's waiting worker
        //       is null, or the result of getting the service worker object that represents registration's waiting
        //       worker in registrationObject's relevant settings object.
        break;

    // 4. Else if target is "active", then:
    case ServiceWorkerRegistrationSlot::Active:
        // 1. Set registration's active worker to source.
        registration.active_worker = source;

        // 2. For each registrationObject in registrationObjects:
        //    1. Queue a task to set the active attribute of registrationObject to null if registration's active worker
        //       is null, or the result of getting the service worker object that represents registration's active
        //       worker in registrationObject's relevant settings object.
        break;
    }

    auto descriptor = descriptor_of(source);
    for_each_process_in_partition(registration.key.partition, [&](ServiceWorkerClientConnection const& connection) {
        connection.visit_open_connection([&](auto& client) {
            client.async_service_worker_registration_state_changed(registration.id, target, descriptor);
        });
    });
}

// https://w3c.github.io/ServiceWorker/#update-state-algorithm
void ServiceWorkerRegistry::update_worker_state(ServiceWorkerRecord& worker, ServiceWorkerState state)
{
    // 1. Assert: state is not "parsed".
    // NOTE: "parsed" is the initial state. A service worker is never updated to this state.
    VERIFY(state != ServiceWorkerState::Parsed);

    // 2. Set worker's state to state.
    worker.state = state;

    // 3. Let settingsObjects be all environment settings objects whose origin is worker's script url's origin.
    // 4. For each settingsObject of settingsObjects, queue a task on settingsObject's responsible event loop in the DOM
    //    manipulation task source to run the following steps:
    // AD-HOC: Only the environments in the partition of the worker's registration can have objects of the worker, so
    //         only the processes that host those are told. They queue the task's steps.
    auto descriptor = ServiceWorkerDescriptor { .id = worker.id, .script_url = worker.script_url, .state = state };
    for_each_process_in_partition(worker.partition, [&](ServiceWorkerClientConnection const& connection) {
        connection.visit_open_connection([&](auto& client) { client.async_service_worker_state_changed(descriptor); });
    });
}

void ServiceWorkerRegistry::for_each_process_in_partition(Partition const& partition, Function<void(ServiceWorkerClientConnection const&)> const& callback) const
{
    WebContentClient::for_each_client([&](WebContentClient& client) {
        if (client.is_private() != m_is_private)
            return IterationDecision::Continue;
        bool hosts_an_environment_in_partition = false;
        client.for_each_page([&](WebContentPage& page) {
            page.for_each_hosted_environment([&](CanonicalEnvironmentSettingsObject const& settings, bool has_cross_site_ancestor) {
                hosts_an_environment_in_partition = partition_of(settings, has_cross_site_ancestor) == partition;
                return hosts_an_environment_in_partition ? IterationDecision::Break : IterationDecision::Continue;
            });
            return hosts_an_environment_in_partition ? IterationDecision::Break : IterationDecision::Continue;
        });
        if (hosts_an_environment_in_partition)
            callback(ServiceWorkerClientConnection { client });
        return IterationDecision::Continue;
    });

    WorkerAgentManager::the().for_each_agent_environment([&](WebWorkerClient& client, CanonicalWorkerEnvironmentSettingsObject const& settings, IsPrivate is_private, Web::HTML::AgentType) {
        if (is_private == m_is_private && partition_of(settings, settings.has_cross_site_ancestor()) == partition)
            callback(ServiceWorkerClientConnection { client });
    });
}

// https://w3c.github.io/ServiceWorker/#set-registration-algorithm
ServiceWorkerRegistry::Registration& ServiceWorkerRegistry::set_registration(Job const& job)
{
    // 1. Run the following steps atomically.
    // 2. Let scopeString be serialized scope with the exclude fragment flag set.
    auto key = registration_key(job.partition, job.scope_url);

    // 3. Let registration be a new service worker registration whose storage key is set to storage key, scope url is
    //    set to scope, and update via cache mode is set to updateViaCache.
    auto registration = make<Registration>(Registration {
        .id = ++m_next_registration_id,
        .key = key,
        .scope_url = job.scope_url,
        .update_via_cache = job.update_via_cache,
        .installing_worker = {},
        .waiting_worker = {},
        .active_worker = {},
        .navigation_preload_enabled = false,
        .navigation_preload_header_value = "true"sv,
    });
    auto& result = *registration;

    // 4. Set registration map[(storage key, scopeString)] to registration.
    m_registration_map.set(move(key), move(registration));

    // 5. Return registration.
    return result;
}

// https://w3c.github.io/ServiceWorker/#get-registration-algorithm
ServiceWorkerRegistry::Registration* ServiceWorkerRegistry::get_registration(Partition const& partition, URL::URL const& scope_url)
{
    // 1. Run the following steps atomically.
    // 2. Let scopeString be the empty string.
    // 3. If scope is not null, set scopeString to serialized scope with the exclude fragment flag set.
    // 4. For each (entry storage key, entry scope) → registration of registration map:
    //    1. If storage key equals entry storage key and scopeString matches entry scope, then return registration.
    // 5. Return null.
    auto registration = m_registration_map.get(registration_key(partition, scope_url));
    return registration.value_or(nullptr);
}

// https://w3c.github.io/ServiceWorker/#scope-match-algorithm
ServiceWorkerRegistry::Registration* ServiceWorkerRegistry::match_service_worker_registration(Partition const& partition, URL::URL const& client_url)
{
    // 1. Run the following steps atomically.
    // 2. Let clientURLString be serialized clientURL.
    auto client_url_string = client_url.serialize();

    // 3. Let matchingScopeString be the empty string.
    Optional<String> matching_scope_string;

    // 4. Let scopeStringSet be an empty list.
    // 5. For each (entry storage key, entry scope) of registration map's keys:
    //    1. If storage key equals entry storage key, then append entry scope to the end of scopeStringSet.
    // 6. Set matchingScopeString to the longest value in scopeStringSet which the value of clientURLString starts with,
    //    if it exists.
    for (auto const& [key, registration] : m_registration_map) {
        if (key.partition != partition || !client_url_string.starts_with_bytes(key.serialized_scope_url))
            continue;
        if (!matching_scope_string.has_value() || key.serialized_scope_url.bytes().size() > matching_scope_string->bytes().size())
            matching_scope_string = key.serialized_scope_url;
    }

    // 7. Let matchingScope be null.
    // 8. If matchingScopeString is not the empty string, then:
    //    1. Set matchingScope to the result of parsing matchingScopeString.
    //    2. Assert: matchingScope's origin and clientURL's origin are same origin.
    // 9. Return the result of running Get Registration given storage key and matchingScope.
    if (!matching_scope_string.has_value())
        return nullptr;
    auto registration = m_registration_map.get(RegistrationKey { partition, matching_scope_string.release_value() });
    VERIFY(registration.has_value());
    VERIFY((*registration)->scope_url.origin().is_same_origin(client_url.origin()));
    return *registration;
}

ServiceWorkerRegistry::Registration* ServiceWorkerRegistry::registration_by_id(ServiceWorkerRegistrationId registration_id)
{
    for (auto& [key, registration] : m_registration_map) {
        if (registration->id == registration_id)
            return registration.ptr();
    }
    for (auto& registration : m_unregistered_registrations) {
        if (registration->id == registration_id)
            return registration.ptr();
    }
    return nullptr;
}

// https://w3c.github.io/ServiceWorker/#dfn-service-worker-registration-unregistered
bool ServiceWorkerRegistry::is_unregistered(Registration const& registration) const
{
    // A service worker registration is said to be unregistered if registration map[this service worker registration's
    // (storage key, serialized scope url)] is not this service worker registration.
    auto registered = m_registration_map.get(registration.key);
    return !registered.has_value() || *registered != &registration;
}

void ServiceWorkerRegistry::remove_registration(Registration& registration)
{
    m_registration_map.remove(registration.key);
}

// https://w3c.github.io/ServiceWorker/#get-newest-worker-algorithm
Optional<ServiceWorkerId> ServiceWorkerRegistry::get_newest_worker(Registration const& registration) const
{
    // 1. Run the following steps atomically.
    // 2. Let newestWorker be null.
    // 3. If registration's installing worker is not null, set newestWorker to registration's installing worker.
    if (registration.installing_worker.has_value())
        return registration.installing_worker;

    // 4. Else if registration's waiting worker is not null, set newestWorker to registration's waiting worker.
    if (registration.waiting_worker.has_value())
        return registration.waiting_worker;

    // 5. Else if registration's active worker is not null, set newestWorker to registration's active worker.
    // 6. Return newestWorker.
    return registration.active_worker;
}

Optional<ServiceWorkerDescriptor> ServiceWorkerRegistry::descriptor_of(Optional<ServiceWorkerId> worker_id) const
{
    if (!worker_id.has_value())
        return {};
    auto const& worker = *m_service_workers.get(*worker_id).value();
    return ServiceWorkerDescriptor {
        .id = worker.id,
        .script_url = worker.script_url,
        .state = worker.state,
    };
}

ServiceWorkerRegistrationSnapshot ServiceWorkerRegistry::snapshot_of(Registration const& registration) const
{
    return ServiceWorkerRegistrationSnapshot {
        .id = registration.id,
        .scope_url = registration.scope_url,
        .update_via_cache = registration.update_via_cache,
        .installing_worker = descriptor_of(registration.installing_worker),
        .waiting_worker = descriptor_of(registration.waiting_worker),
        .active_worker = descriptor_of(registration.active_worker),
    };
}

// https://w3c.github.io/ServiceWorker/#navigator-service-worker-getRegistration
void ServiceWorkerRegistry::get_registration(ServiceWorkerClientEnvironment const& client, ServiceWorkerRequestId request_id, URL::URL client_url)
{
    // NB: The client's process runs the other steps, and settles its promise with what step 8.1 finds here.
    // 2. Let storage key be the result of running obtain a storage key given client.
    auto partition = partition_of(client.settings, client.has_cross_site_ancestor);

    // 8. Run the following substeps in parallel:
    // 1. Let registration be the result of running Match Service Worker Registration given storage key and clientURL.
    Vector<ServiceWorkerRegistrationSnapshot> registrations;
    if (partition.has_value()) {
        if (auto* registration = match_service_worker_registration(*partition, client_url))
            registrations.append(snapshot_of(*registration));
    }

    // 2. Queue a task on promise's relevant settings object's responsible event loop, using the DOM manipulation task
    //    source, to run the following steps:
    client.connection.service_worker_registration_lookup_finished(request_id, registrations);
}

// https://w3c.github.io/ServiceWorker/#navigator-service-worker-getRegistrations
void ServiceWorkerRegistry::get_registrations(ServiceWorkerClientEnvironment const& client, ServiceWorkerRequestId request_id)
{
    // NB: The client's process runs the other steps, and settles its promise with what step 4 finds here.
    // 2. Let client storage key be the result of running obtain a storage key given client.
    auto partition = partition_of(client.settings, client.has_cross_site_ancestor);

    // 4. Run the following steps in parallel:
    // 1. Let registrations be a new list.
    Vector<Registration const*> registrations;

    // 2. For each (storage key, scope) → registration of registration map:
    //    1. If storage key equals client storage key, then append registration to registrations.
    if (partition.has_value()) {
        for (auto const& [key, registration] : m_registration_map) {
            if (key.partition == *partition)
                registrations.append(registration.ptr());
        }
    }

    // NB: The registration map is ordered by when each registration was set.
    quick_sort(registrations, [](auto const* a, auto const* b) { return a->id < b->id; });

    Vector<ServiceWorkerRegistrationSnapshot> snapshots;
    snapshots.ensure_capacity(registrations.size());
    for (auto const* registration : registrations)
        snapshots.unchecked_append(snapshot_of(*registration));

    // 3. Queue a task on promise's relevant settings object's responsible event loop, using the DOM manipulation task
    //    source, to run the following steps:
    client.connection.service_worker_registration_lookup_finished(request_id, snapshots);
}

// https://w3c.github.io/ServiceWorker/#navigator-service-worker-ready
void ServiceWorkerRegistry::get_ready_registration(ServiceWorkerClientEnvironment const& client, ServiceWorkerRequestId request_id, URL::URL creation_url)
{
    // NB: The client's process runs the other steps, and resolves its ready promise with what step 3.3 finds here. The
    //     creation URL is the one the client's process names, which can only change which of its own origin's
    //     registrations resolve the client's promise.
    // 3. If readyPromise is pending, run the following substeps in parallel:
    // 1. Let client by this's service worker client.
    // 2. Let storage key be the result of running obtain a storage key given client.
    auto partition = partition_of(client.settings, client.has_cross_site_ancestor);
    if (!partition.has_value())
        return;

    // 3. Let registration be the result of running Match Service Worker Registration given storage key and client's
    //    creation URL.
    auto* registration = match_service_worker_registration(*partition, creation_url);

    // 4. If registration is not null, and registration's active worker is not null, queue a task on readyPromise's
    //    relevant settings object's responsible event loop, using the DOM manipulation task source, to resolve
    //    readyPromise with the result of getting the service worker registration object that represents registration
    //    in readyPromise's relevant settings object.
    if (registration && registration->active_worker.has_value()) {
        client.connection.visit_open_connection([&](auto& connection) {
            connection.async_service_worker_ready(request_id, snapshot_of(*registration));
        });
        return;
    }

    // NOTE: The returned ready promise will never reject. If it does not resolve in this algorithm, it will eventually
    //       resolve when a matching service worker registration is registered and its active worker is set. (See the
    //       relevant Activate algorithm step.)
    m_ready_requests.append(ReadyRequest {
        .connection = client.connection,
        .environment_id = client.settings.id(),
        .request_id = request_id,
        .partition = partition.release_value(),
        .creation_url = move(creation_url),
    });
}

void ServiceWorkerRegistry::clear_registrations_for_testing(URL::Origin const& top_level_origin)
{
    auto top_level_site = URL::Site::serialize_for_partitioning(top_level_origin);
    auto is_of_test = [&](Partition const& partition) {
        return partition.top_level_site == top_level_site || partition.storage_key.origin.is_same_origin(top_level_origin);
    };

    // NB: A job that waits behind the one that runs would make a registration after the test's are cleared.
    for (auto& [key, job_queue] : m_scope_to_job_queue_map) {
        if (is_of_test(key.partition) && job_queue.size() > 1)
            job_queue.shrink(1);
    }

    Vector<RegistrationKey> registration_keys;
    for (auto& [key, registration] : m_registration_map) {
        if (is_of_test(key.partition))
            registration_keys.append(key);
    }
    for (auto const& key : registration_keys) {
        if (auto registration = m_registration_map.take(key); registration.has_value())
            clear_registration(**registration);
    }

    Vector<NonnullOwnPtr<Registration>> unregistered_registrations_of_test;
    for (size_t i = 0; i < m_unregistered_registrations.size();) {
        if (is_of_test(m_unregistered_registrations[i]->key.partition))
            unregistered_registrations_of_test.append(m_unregistered_registrations.take(i));
        else
            ++i;
    }
    for (auto& registration : unregistered_registrations_of_test)
        clear_registration(*registration);
}

}
