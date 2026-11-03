/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
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

private:
    void set_up_service_worker_global_scope(GC::Ref<Web::Page>);
    void fetch_script(GC::Ref<Web::Page>);

    Web::ServiceWorker::ServiceWorkerStartRequest m_request;

    GC::Root<Web::HTML::WorkerDebugConsoleClient> m_console;
    GC::Root<Web::ServiceWorker::ServiceWorkerGlobalScope> m_global_scope;
    GC::Root<Web::HTML::WorkerEnvironmentSettingsObject> m_settings_object;
};

}
