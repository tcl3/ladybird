/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibCore/ImmutableBytes.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/ResponseView.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWebCommon/ServiceWorker/ServiceWorkerTypes.h>

namespace Web::ServiceWorker {

// https://w3c.github.io/ServiceWorker/#serviceworkerglobalscope
class WEB_API ServiceWorkerGlobalScope : public HTML::WorkerGlobalScope {
    WEB_WRAPPABLE(ServiceWorkerGlobalScope, HTML::WorkerGlobalScope);
    GC_DECLARE_ALLOCATOR(ServiceWorkerGlobalScope);

public:
    virtual ~ServiceWorkerGlobalScope() override;

    void set_oninstall(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> oninstall();

    void set_onactivate(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onactivate();

    void set_onfetch(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onfetch();

    void set_onmessage(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onmessage();

    void set_onmessageerror(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onmessageerror();

    [[nodiscard]] GC::Ref<CookieStore::CookieStore> cookie_store();

    GC::Ref<WebIDL::Promise> skip_waiting();
    void skip_waiting_finished(ServiceWorkerRequestId);

    GC::Ref<ServiceWorkerRegistration> registration();
    GC::Ref<ServiceWorker> service_worker();

    void set_service_worker(ServiceWorkerDescriptor service_worker) { m_service_worker = move(service_worker); }
    void set_registration(ServiceWorkerRegistrationSnapshot registration) { m_registration = move(registration); }
    void service_worker_state_changed(ServiceWorkerDescriptor const&);
    void registration_state_changed(ServiceWorkerRegistrationSlot, Optional<ServiceWorkerDescriptor>);
    void prune_script_resource_map();

    virtual HTML::PerformTheFetchHook import_scripts_perform_the_fetch_hook() override;

protected:
    explicit ServiceWorkerGlobalScope(GC::Ref<Web::Page>);

private:
    virtual void initialize_web_interfaces_impl() override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<CookieStore::CookieStore> m_cookie_store;

    // https://w3c.github.io/ServiceWorker/#serviceworkerglobalscope-service-worker
    // NB: The UI process holds the service worker and its containing registration, and describes them here.
    ServiceWorkerDescriptor m_service_worker;
    ServiceWorkerRegistrationSnapshot m_registration;

    struct ScriptResource {
        GC::Ref<Fetch::Infrastructure::ResponseView> response;
        Core::ImmutableBytes body;
    };

    // https://w3c.github.io/ServiceWorker/#dfn-script-resource-map
    OrderedHashMap<URL::URL, ScriptResource> m_script_resource_map;

    // https://w3c.github.io/ServiceWorker/#dfn-set-of-used-scripts
    Vector<URL::URL> m_set_of_used_scripts;

    HashMap<ServiceWorkerRequestId, GC::Ref<WebIDL::Promise>> m_skip_waiting_promises;
    ServiceWorkerRequestId m_next_skip_waiting_request_id { 0 };
};

}
