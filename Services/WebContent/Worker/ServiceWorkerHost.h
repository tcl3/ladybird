/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/RefCounted.h>
#include <AK/Weakable.h>
#include <LibCore/Forward.h>
#include <LibCore/ImmutableBytes.h>
#include <LibGC/Root.h>
#include <LibIPC/TransportHandle.h>
#include <LibWeb/Forward.h>
#include <LibWeb/ServiceWorker/FetchEventTypes.h>
#include <LibWebCommon/HTML/SerializationRecords.h>
#include <LibWebCommon/ServiceWorker/ServiceWorkerTypes.h>
#include <WebContent/Worker/Forward.h>

namespace WebWorker {

// https://w3c.github.io/ServiceWorker/#dfn-service-worker
class ServiceWorkerHost
    : public RefCounted<ServiceWorkerHost>
    , public Weakable<ServiceWorkerHost> {
public:
    GC_HOLDS_NO_CELL_EDGES;

    explicit ServiceWorkerHost(Web::ServiceWorker::ServiceWorkerStartRequest);
    ~ServiceWorkerHost();

    void start(GC::Ref<Web::Page>);

    // https://w3c.github.io/ServiceWorker/#terminate-service-worker
    void terminate();
    void run_closing_steps();

    void service_worker_state_changed(Web::ServiceWorker::ServiceWorkerDescriptor const&);
    void registration_state_changed(Web::ServiceWorker::ServiceWorkerRegistrationId, Web::ServiceWorker::ServiceWorkerRegistrationSlot target, Optional<Web::ServiceWorker::ServiceWorkerDescriptor> const& source);
    void dispatch_lifecycle_event(Web::ServiceWorker::ServiceWorkerLifecycleEvent, AK::Duration timeout);
    void dispatch_message_event(Web::ServiceWorker::ServiceWorkerMessageSource, URL::Origin const&, Web::HTML::SerializedTransferRecord, AK::Duration timeout);
    void skip_waiting_finished(Web::ServiceWorker::ServiceWorkerRequestId);

    void connect_fetch_channel(IPC::TransportHandle, Web::HTML::EnvironmentId client);
    void fetch_connection_closed(ServiceWorkerFetchConnection&);
    void dispatch_fetch_event(ServiceWorkerFetchConnection&, Web::ServiceWorker::FetchEventId, Web::ServiceWorker::FetchEventRequest);
    void allow_fetch_response_body(ServiceWorkerFetchConnection&, Web::ServiceWorker::FetchEventId, u64 total_byte_count);
    void cancel_fetch(ServiceWorkerFetchConnection&, Web::ServiceWorker::FetchEventId);
    void fetch_event_did_settle(u64 dispatch_id, Web::ServiceWorker::FetchEventOutcome, JS::Object* response);

private:
    void set_up_service_worker_global_scope(GC::Ref<Web::Page>);
    void fetch_script(GC::Ref<Web::Page>);
    void run_stored_scripts(GC::Ref<Web::Page>);
    bool finish_setting_up_service_worker_global_scope(GC::Ref<Web::Fetch::Infrastructure::ResponseView>);
    void run_service_worker(GC::Ref<Web::Page>, GC::Ref<Web::HTML::Script>, GC::Ref<Web::Fetch::Infrastructure::ResponseView>);
    void lifecycle_event_finished(bool failed);
    void pending_events_changed(bool has_pending_events);
    void record_scripts();

    Web::ServiceWorker::ServiceWorkerStartRequest m_request;

    GC::Root<Web::Page> m_page;
    GC::Root<Web::HTML::WorkerDebugConsoleClient> m_console;
    GC::Root<Web::ServiceWorker::ServiceWorkerGlobalScope> m_global_scope;
    GC::Root<Web::HTML::WorkerEnvironmentSettingsObject> m_settings_object;
    GC::Root<Web::Fetch::Infrastructure::ResponseView> m_main_script_response;
    Core::ImmutableBytes m_main_script_body;
    bool m_restarted_from_stored_scripts { false };

    // A fetch event of a client, from when the client's process sends it until the worker's response body ends.
    struct FetchEventInFlight {
        AK_ALLOC_WITH_KMALLOC;

        WeakPtr<ServiceWorkerFetchConnection> connection;
        Web::ServiceWorker::FetchEventId fetch_id { 0 };
        u64 dispatch_id { 0 };
        Optional<Web::ServiceWorker::FetchEventRequest> request_waiting_for_dispatch;

        // NB: The response's body, as far as it was read and the client allowed it to be delivered.
        GC::Root<Web::Streams::ReadableStreamDefaultReader> body_reader;
        ByteBuffer unsent_body_bytes;
        u64 allowed_byte_count { 0 };
        u64 sent_byte_count { 0 };
        Optional<bool> body_end;
    };

    bool can_dispatch_fetch_events() const;
    void dispatch_waiting_fetch_events();
    void run_fetch_event_task(FetchEventInFlight&);
    void send_response_body_bytes(FetchEventInFlight&);
    FetchEventInFlight* fetch_event_in_flight(ServiceWorkerFetchConnection const&, Web::ServiceWorker::FetchEventId);
    FetchEventInFlight* fetch_event_with_dispatch_id(u64 dispatch_id);
    void forget_fetch_event(FetchEventInFlight const&);

    Vector<NonnullRefPtr<ServiceWorkerFetchConnection>> m_fetch_connections;
    Vector<NonnullOwnPtr<FetchEventInFlight>> m_fetch_events;
    u64 m_next_fetch_dispatch_id { 0 };
    bool m_is_running { false };

    Optional<Web::ServiceWorker::ServiceWorkerLifecycleEvent> m_lifecycle_event_in_flight;
    RefPtr<Core::Timer> m_lifecycle_event_deadline_timer;
    HashMap<u64, NonnullRefPtr<Core::Timer>> m_extended_event_deadline_timers;
};

}
