/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/ConsoleObject.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/ServiceWorkerGlobalScope.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/FileAPI/BlobURLStore.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Scripting/RemoteEnvironmentSettings.h>
#include <LibWeb/HTML/Scripting/WorkerEnvironmentSettingsObject.h>
#include <LibWeb/HTML/WorkerDebugConsoleClient.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/ServiceWorker/ServiceWorkerGlobalScope.h>
#include <LibWeb/ServiceWorker/Update.h>
#include <WebContent/Worker/PageHost.h>
#include <WebContent/Worker/ServiceWorkerHost.h>

namespace WebWorker {

ServiceWorkerHost::ServiceWorkerHost(Web::ServiceWorker::ServiceWorkerStartRequest request)
    : m_request(move(request))
{
}

ServiceWorkerHost::~ServiceWorkerHost() = default;

void ServiceWorkerHost::start(GC::Ref<Web::Page> page)
{
    set_up_service_worker_global_scope(page);
    fetch_script(page);
}

// https://w3c.github.io/ServiceWorker/#terminate-service-worker
void ServiceWorkerHost::terminate()
{
    // 1. Run the following steps in parallel with serviceWorker's main loop:
    //    1. Let serviceWorkerGlobalScope be serviceWorker's global object.
    //    2. Set serviceWorkerGlobalScope's closing flag to true.
    //    3. Remove all the items from serviceWorker's set of extended events.
    //    4. If there are any tasks, whose task source is either the handle fetch task queue or the handle functional
    //       event task queue, queued in serviceWorkerGlobalScope's event loop's task queues, queue them to
    //       serviceWorker's containing service worker registration's corresponding task queues in the same order using
    //       their original task sources, and discard all the tasks (including tasks whose task source is neither the
    //       handle fetch task queue nor the handle functional event task queue) from serviceWorkerGlobalScope's event
    //       loop's task queues without processing them.
    //    5. Abort the script currently running in serviceWorker.
    //    6. Set serviceWorker's start status to null.
    // NB: The UI process holds the start status and the registration's task queues. Closing the worker discards its
    //     tasks, and aborts its script.
    if (m_global_scope) {
        m_global_scope->terminate_a_worker();
        return;
    }
    JS::VM::the().request_interrupt();
    Web::HTML::current_event_loop().destroy_once_script_has_finished();
}

// NB: These are the closing steps that run a worker has for the worker's global scope, which go with the service worker as
//     well.
void ServiceWorkerHost::run_closing_steps()
{
    if (m_global_scope) {
        m_global_scope->clear_map_of_active_timers();
        m_global_scope->forcibly_close_all_event_sources();
        Web::HTML::MessagePort::disentangle_ports_of_realm(Web::HTML::relevant_realm(*m_global_scope));
        if (m_settings_object)
            Web::FileAPI::remove_entries_of_environment(*m_settings_object, *m_global_scope->page());
    }

    // NB: The roots have to go before the heap does.
    m_console = nullptr;
    m_global_scope = nullptr;
    m_settings_object = nullptr;
}

// https://w3c.github.io/ServiceWorker/#setup-serviceworkerglobalscope
void ServiceWorkerHost::set_up_service_worker_global_scope(GC::Ref<Web::Page> page)
{
    // 1. Let unsafeCreationTime be the unsafe shared current time.
    auto unsafe_creation_time = Web::HighResolutionTime::unsafe_shared_current_time();

    // 2. If serviceWorker is running, then return serviceWorker's global object.
    // 3. If serviceWorker's state is "redundant", then return null.
    // 4. If serviceWorker's global object is not null, then return serviceWorker's global object.
    // 5. Assert: serviceWorker's start status is null.
    // NB: A service worker's process sets its global object up once, when it starts, before it fetches a new worker's
    //     script with the global object's environment settings object.

    // 6. Let setupFailed be false.
    // 7. Let globalObject be null.

    // 8. Let agent be the result of obtaining a service worker agent, and run the following steps in that context:
    // NB: This process's agent is a service worker agent.

    // 1. Let realmExecutionContext be the result of creating a new realm given agent and the following customizations:
    //    - For the global object, create a new ServiceWorkerGlobalScope object. Let workerGlobalScope be the created
    //      object.
    auto realm_execution_context = Web::Bindings::create_a_new_javascript_realm(
        JS::VM::the(),
        Web::HTML::CrossOriginIsolationMode::None,
        [page](JS::Realm& realm) -> GC::Ref<JS::Object> {
            auto worker_global_scope = GC::Heap::the().allocate<Web::ServiceWorker::ServiceWorkerGlobalScope>(page);
            return Web::Bindings::create_global_object_wrapper(realm, GC::Ref { *worker_global_scope });
        },
        nullptr);
    auto worker_global_scope = GC::Ref { Web::Bindings::impl_from<Web::ServiceWorker::ServiceWorkerGlobalScope>(realm_execution_context->realm->global_object()) };
    m_global_scope = worker_global_scope;

    // 4. Set workerGlobalScope's url to serviceWorker's script url.
    // AD-HOC: This is set before the settings object, whose API base URL and creation URL are the global object's url.
    worker_global_scope->set_url(m_request.worker.script_url);

    // 2. Let settingsObject be a new environment settings object whose algorithms are defined as follows:
    //    The realm execution context
    //        Return realmExecutionContext.
    //    The module map
    //        Return workerGlobalScope's module map.
    //    The API base URL
    //        Return serviceWorker's script url.
    //    The origin
    //        Return its registering service worker client's origin.
    //    The has cross-site ancestor
    //        Return its registering service worker client's has cross-site ancestor.
    //    The policy container
    //        Return workerGlobalScope's policy container.
    //    The time origin
    //        Return the result of coarsening unsafeCreationTime given workerGlobalScope's cross-origin isolated
    //        capability.
    // 3. Set settingsObject's id to a new unique opaque string, creation URL to serviceWorker's script url, top-level
    //    creation URL to null, top-level origin to an implementation-defined value, target browsing context to null, and
    //    active service worker to null.
    // NB: The UI process generates the id, as it holds the worker's environment too. The top-level origin is that of
    //     the client that registered the worker.
    m_settings_object = Web::HTML::WorkerEnvironmentSettingsObject::create_for_service_worker(page, move(realm_execution_context), m_request.origin, m_request.has_cross_site_ancestor, m_request.top_level_origin, unsafe_creation_time, m_request.environment_id);

    // AD-HOC: Create a console object for the worker.
    auto& console_object = *m_settings_object->realm().intrinsics().console_object();
    m_console = GC::Heap::the().allocate<Web::HTML::WorkerDebugConsoleClient>(console_object.console());
    console_object.console().set_client(*m_console);

    // FIXME: 5. Set workerGlobalScope's policy container to serviceWorker's script resource's policy container.
    // FIXME: 6. Set workerGlobalScope's type to serviceWorker's type.
    // FIXME: 7. Create a new WorkerLocation object and associate it with workerGlobalScope.
    // FIXME: 8. If the run CSP initialization for a global object algorithm returns "Blocked" when executed upon
    //           workerGlobalScope, set setupFailed to true and abort these steps.

    // 9. Set globalObject to workerGlobalScope.

    // 9. Wait for globalObject is not null, or for setupFailed to be true.
    // 10. If setupFailed is true, then return null.
    // 11. Return globalObject.
}

void ServiceWorkerHost::fetch_script(GC::Ref<Web::Page> page)
{
    auto job_client = Web::HTML::RemoteEnvironmentSettings::create(m_request.script_fetch.job_client);
    Web::ServiceWorker::UpdateScriptFetch job {
        .script_url = m_request.worker.script_url,
        .scope_url = m_request.registration.scope_url,
        .worker_type = m_request.worker_type,
        .update_via_cache = m_request.registration.update_via_cache,
        .force_bypass_cache = m_request.script_fetch.force_bypass_cache,
    };
    Web::ServiceWorker::fetch_the_script_of_a_new_service_worker(job_client, *m_settings_object, move(job), GC::create_function(GC::Heap::the(), GC::captures(page), [](auto& page, GC::Ptr<Web::HTML::Script> script, Optional<Web::ServiceWorker::ServiceWorkerJobError> rejection) {
        Web::ServiceWorker::ServiceWorkerScriptFetchResult result;
        if (!script)
            result = rejection.value_or(Web::ServiceWorker::ServiceWorkerJobError::TypeError);
        as<PageHost>(page->client()).did_fetch_service_worker_script(result);
    }));
}

}
