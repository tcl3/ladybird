/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/RefCounted.h>
#include <LibCore/Forward.h>
#include <LibGC/Root.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/HTML/SerializationRecords.h>
#include <LibWebCommon/ServiceWorker/ServiceWorkerTypes.h>
#include <WebContent/Worker/Forward.h>

namespace WebWorker {

// https://w3c.github.io/ServiceWorker/#dfn-service-worker
class ServiceWorkerHost : public RefCounted<ServiceWorkerHost> {
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

private:
    void set_up_service_worker_global_scope(GC::Ref<Web::Page>);
    void fetch_script(GC::Ref<Web::Page>);
    bool finish_setting_up_service_worker_global_scope(GC::Ref<Web::Fetch::Infrastructure::ResponseView>);
    void run_service_worker(GC::Ref<Web::Page>, GC::Ref<Web::HTML::Script>, GC::Ref<Web::Fetch::Infrastructure::ResponseView>);
    void lifecycle_event_finished(bool failed);
    void pending_events_changed(bool has_pending_events);

    Web::ServiceWorker::ServiceWorkerStartRequest m_request;

    GC::Root<Web::Page> m_page;
    GC::Root<Web::HTML::WorkerDebugConsoleClient> m_console;
    GC::Root<Web::ServiceWorker::ServiceWorkerGlobalScope> m_global_scope;
    GC::Root<Web::HTML::WorkerEnvironmentSettingsObject> m_settings_object;

    Optional<Web::ServiceWorker::ServiceWorkerLifecycleEvent> m_lifecycle_event_in_flight;
    RefPtr<Core::Timer> m_lifecycle_event_deadline_timer;
    HashMap<u64, NonnullRefPtr<Core::Timer>> m_extended_event_deadline_timers;
};

}
