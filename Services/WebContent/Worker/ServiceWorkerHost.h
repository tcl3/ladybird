/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <LibCore/Forward.h>
#include <LibGC/Root.h>
#include <LibWeb/Forward.h>
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
    void dispatch_lifecycle_event(Web::ServiceWorker::ServiceWorkerLifecycleEvent, AK::Duration timeout);
    void skip_waiting_finished(Web::ServiceWorker::ServiceWorkerRequestId);

private:
    void set_up_service_worker_global_scope(GC::Ref<Web::Page>);
    void fetch_script(GC::Ref<Web::Page>);
    bool finish_setting_up_service_worker_global_scope(GC::Ref<Web::Fetch::Infrastructure::ResponseView>);
    void run_service_worker(GC::Ref<Web::Page>, GC::Ref<Web::HTML::Script>, GC::Ref<Web::Fetch::Infrastructure::ResponseView>);
    void lifecycle_event_finished(bool failed);

    Web::ServiceWorker::ServiceWorkerStartRequest m_request;

    GC::Root<Web::Page> m_page;
    GC::Root<Web::HTML::WorkerDebugConsoleClient> m_console;
    GC::Root<Web::ServiceWorker::ServiceWorkerGlobalScope> m_global_scope;
    GC::Root<Web::HTML::WorkerEnvironmentSettingsObject> m_settings_object;

    Optional<Web::ServiceWorker::ServiceWorkerLifecycleEvent> m_lifecycle_event_in_flight;
    RefPtr<Core::Timer> m_lifecycle_event_deadline_timer;
};

}
