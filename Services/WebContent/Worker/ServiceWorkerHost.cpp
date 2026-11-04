/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/NeverDestroyed.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibGC/Heap.h>
#include <LibHTTP/HeaderList.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/ConsoleObject.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/ServiceWorkerGlobalScope.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/AbortController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/ResponseView.h>
#include <LibWeb/Fetch/Request.h>
#include <LibWeb/Fetch/Response.h>
#include <LibWeb/FileAPI/BlobURLStore.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/Fetching.h>
#include <LibWeb/HTML/Scripting/ModuleScript.h>
#include <LibWeb/HTML/Scripting/RemoteEnvironmentSettings.h>
#include <LibWeb/HTML/Scripting/WorkerEnvironmentSettingsObject.h>
#include <LibWeb/HTML/WorkerDebugConsoleClient.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/ServiceWorker/NavigationPreload.h>
#include <LibWeb/ServiceWorker/ServiceWorkerGlobalScope.h>
#include <LibWeb/ServiceWorker/Update.h>
#include <LibWeb/Streams/ReadableStreamDefaultReader.h>
#include <WebContent/Worker/PageHost.h>
#include <WebContent/Worker/ServiceWorkerFetchConnection.h>
#include <WebContent/Worker/ServiceWorkerHost.h>

extern "C" {
void web_rust_service_worker_dispatch_lifecycle_event(JSVM*, void* realm, Web::DOM::EventTarget* global, u8 kind, void* ctx, void (*on_finished)(void*, bool failed));
void web_rust_service_worker_time_out_lifecycle_event(JSVM*);
u64 web_rust_service_worker_dispatch_fetch_event(JSVM*, void* realm, Web::DOM::EventTarget* global, void* request, ReadonlyBytes client_id, ReadonlyBytes resulting_client_id, ReadonlyBytes replaces_client_id, void* preload_response, void* ctx, void (*on_settled)(void*, u8 outcome, void* response), void* pending_events_ctx, void (*on_pending_events_changed)(void*, bool));
}

namespace WebWorker {

// NB: A process runs one service worker, whose host the fetch events it dispatches settle with.
static WeakPtr<ServiceWorkerHost>& fetch_event_host()
{
    // NB: A worker's thread runs one service worker.
    static thread_local NeverDestroyed<WeakPtr<ServiceWorkerHost>> host;
    return *host;
}

ServiceWorkerHost::ServiceWorkerHost(Web::ServiceWorker::ServiceWorkerStartRequest request)
    : m_request(move(request))
{
    fetch_event_host() = make_weak_ptr();
}

ServiceWorkerHost::~ServiceWorkerHost() = default;

void ServiceWorkerHost::start(GC::Ref<Web::Page> page)
{
    m_page = page;
    set_up_service_worker_global_scope(page);
    if (m_request.stored_scripts.is_empty())
        fetch_script(page);
    else
        run_stored_scripts(page);
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

    if (m_lifecycle_event_deadline_timer)
        m_lifecycle_event_deadline_timer->stop();
    m_lifecycle_event_deadline_timer = nullptr;
    for (auto& [event_id, timer] : m_extended_event_deadline_timers)
        timer->stop();
    m_extended_event_deadline_timers.clear();

    // NB: A fetch event in flight holds the reader of its response's body. The channels of clients close with the
    //     worker, as their sockets are watched by this thread.
    m_fetch_events.clear();
    fetch_event_host() = nullptr;
    auto fetch_connections = move(m_fetch_connections);
    for (auto& connection : fetch_connections)
        connection->shutdown();

    // NB: The roots have to go before the heap does.
    m_page = nullptr;
    m_console = nullptr;
    m_global_scope = nullptr;
    m_settings_object = nullptr;
    m_main_script_response = nullptr;
    m_main_script_body = {};
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

    m_global_scope->set_service_worker(m_request.worker);
    m_global_scope->set_registration(m_request.registration);

    // NB: The rest of the steps run once the script resource is fetched.
}

// https://w3c.github.io/ServiceWorker/#setup-serviceworkerglobalscope
bool ServiceWorkerHost::finish_setting_up_service_worker_global_scope(GC::Ref<Web::Fetch::Infrastructure::ResponseView> response)
{
    auto& worker_global_scope = *m_global_scope;

    // 8.5. Set workerGlobalScope's policy container to serviceWorker's script resource's policy container.
    worker_global_scope.set_policy_container(Web::HTML::create_a_policy_container_from_a_fetch_response(response, *m_settings_object));

    // 8.6. Set workerGlobalScope's type to serviceWorker's type.
    worker_global_scope.set_type(m_request.worker_type);

    // 8.7. Create a new WorkerLocation object and associate it with workerGlobalScope.
    worker_global_scope.set_location(GC::Heap::the().allocate<Web::HTML::WorkerLocation>(worker_global_scope));

    // 8.8. If the run CSP initialization for a global object algorithm returns "Blocked" when executed upon
    //      workerGlobalScope, set setupFailed to true and abort these steps.
    // 8.9. Set globalObject to workerGlobalScope.
    // 9. Wait for globalObject is not null, or for setupFailed to be true.
    // 10. If setupFailed is true, then return null.
    // 11. Return globalObject.
    return worker_global_scope.run_csp_initialization() != Web::ContentSecurityPolicy::Directives::Directive::Result::Blocked;
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
        .newest_worker_is_stale = m_request.newest_worker_script_url.has_value() && m_request.registration_is_stale,
    };
    Web::ServiceWorker::fetch_the_script_of_a_new_service_worker(job_client, *m_settings_object, move(job), GC::create_function(GC::Heap::the(), GC::captures(NonnullRefPtr { *this }, page), [](auto& host, auto& page, GC::Ptr<Web::HTML::Script> script, GC::Ptr<Web::Fetch::Infrastructure::ResponseView> response, Core::ImmutableBytes body, Vector<Web::ServiceWorker::ServiceWorkerScriptResource> imported_module_scripts, Optional<Web::ServiceWorker::ServiceWorkerJobError> rejection) {
        if (!script || !response) {
            as<PageHost>(page->client()).did_fetch_service_worker_script(rejection.value_or(Web::ServiceWorker::ServiceWorkerJobError::TypeError));
            return;
        }
        host->m_main_script_response = response;
        host->m_main_script_body = move(body);
        host->m_imported_module_scripts = move(imported_module_scripts);
        host->check_for_updated_resources(page, *script, *response);
    }));
}

// https://w3c.github.io/ServiceWorker/#update-algorithm
void ServiceWorkerHost::check_for_updated_resources(GC::Ref<Web::Page> page, GC::Ref<Web::HTML::Script> script, GC::Ref<Web::Fetch::Infrastructure::ResponseView> response)
{
    auto const& script_url = m_request.worker.script_url;

    // 7. [...] 20. Set hasUpdatedResources to true if any of the following are true:
    //              - newestWorker is null.
    //              - newestWorker's script url is not url or newestWorker's type is not job's worker type.
    //              - newestWorker's script resource map[url]'s body is not byte-for-byte identical with response's
    //                body.
    bool has_updated_resources = !m_request.newest_worker_script_url.has_value()
        || *m_request.newest_worker_script_url != script_url
        || m_request.newest_worker_type != m_request.worker_type;
    if (!has_updated_resources) {
        auto stored_script = m_request.newest_worker_scripts.find_if([&](auto const& resource) { return resource.url == script_url; });
        has_updated_resources = stored_script == m_request.newest_worker_scripts.end() || stored_script->body.bytes() != m_main_script_body.bytes();
    }
    // AD-HOC: An update also finds the scripts of a module worker updated if the module scripts that its script imports
    //         are not byte-for-byte identical with the newest worker's, as in other engines.
    if (!has_updated_resources && m_request.worker_type == Web::HTML::WorkerType::Module) {
        has_updated_resources = m_imported_module_scripts.size() + 1 != m_request.newest_worker_scripts.size()
            || any_of(m_imported_module_scripts, [&](auto const& module_script) {
                   auto stored_script = m_request.newest_worker_scripts.find_if([&](auto const& resource) { return resource.url == module_script.url; });
                   return stored_script == m_request.newest_worker_scripts.end() || stored_script->body != module_script.body;
               });
        if (!has_updated_resources) {
            // 9. If hasUpdatedResources is false, then:
            // NB: The UI process runs the substeps, and stops this process.
            as<PageHost>(page->client()).did_find_service_worker_scripts_up_to_date();
            return;
        }
    }
    if (has_updated_resources) {
        run_service_worker(page, script, response);
        return;
    }

    // 21. If hasUpdatedResources is false and newestWorker's classic scripts imported flag is set, then:
    // NB: The newest worker imported classic scripts if its process recorded scripts other than its main script.
    Web::ServiceWorker::UpdateImportedScriptsCheck check {
        .script_url = script_url,
        .update_via_cache = m_request.registration.update_via_cache,
        .force_bypass_cache = m_request.script_fetch.force_bypass_cache,
        .registration_is_stale = m_request.registration_is_stale,
    };
    Web::ServiceWorker::check_imported_scripts_for_update(*m_settings_object, m_request.newest_worker_scripts, check, GC::create_function(GC::Heap::the(), GC::captures(NonnullRefPtr { *this }, page, script, response), [](auto& host, auto& page, auto& script, auto& response, bool imported_scripts_changed) {
        if (imported_scripts_changed) {
            host->run_service_worker(page, script, response);
            return;
        }

        // 9. If hasUpdatedResources is false, then:
        // NB: The UI process runs the substeps, and stops this worker.
        as<PageHost>(page->client()).did_find_service_worker_scripts_up_to_date();
    }));
}

// https://w3c.github.io/ServiceWorker/#run-service-worker-algorithm
// NB: The script resource and script resource map of an installed worker are the scripts that its process recorded
//     when the worker installed.
void ServiceWorkerHost::run_stored_scripts(GC::Ref<Web::Page> page)
{
    Optional<Web::ServiceWorker::ServiceWorkerScriptResource> main_script;
    Vector<Web::ServiceWorker::ServiceWorkerScriptResource> imported_scripts;
    for (auto& resource : m_request.stored_scripts) {
        if (!main_script.has_value() && resource.url == m_request.worker.script_url)
            main_script = move(resource);
        else
            imported_scripts.append(move(resource));
    }
    m_request.stored_scripts.clear();
    m_restarted_from_stored_scripts = true;
    if (!main_script.has_value()) {
        as<PageHost>(page->client()).did_run_service_worker(false, {});
        return;
    }

    if (m_request.worker_type == Web::HTML::WorkerType::Module) {
        m_imported_module_scripts = move(imported_scripts);
        run_stored_module_scripts(page, main_script.release_value());
        return;
    }

    m_global_scope->restore_imported_script_resources(imported_scripts);
    auto response = Web::ServiceWorker::response_from_script_resource(*main_script);
    auto source_text = Utf16String::from_utf8_with_replacement_character(StringView { main_script->body.bytes() });
    auto script = Web::HTML::ClassicScript::create(main_script->url.to_byte_string(), source_text, *m_settings_object, main_script->url);
    m_main_script_response = response;
    m_main_script_body = Core::ImmutableBytes::adopt(move(main_script->body));
    run_service_worker(page, script, response);
}

// https://w3c.github.io/ServiceWorker/#run-service-worker-algorithm
// NB: A module worker's script resource is a module script, whose graph the worker's process creates again from the
//     scripts that it recorded.
void ServiceWorkerHost::run_stored_module_scripts(GC::Ref<Web::Page> page, Web::ServiceWorker::ServiceWorkerScriptResource main_script)
{
    auto response = Web::ServiceWorker::response_from_script_resource(main_script);
    m_main_script_response = response;
    m_main_script_body = Core::ImmutableBytes::adopt(move(main_script.body));

    Web::HTML::PerformTheFetchHook perform_the_fetch_hook = GC::create_function(GC::Heap::the(), GC::captures(NonnullRefPtr { *this }), [](auto& host, NonnullRefPtr<Web::Fetch::Infrastructure::Request> request, Web::HTML::TopLevelModule top_level, Web::HTML::ProcessCustomFetchResponse process_custom_fetch_response) -> Web::WebIDL::ExceptionOr<void> {
        if (top_level == Web::HTML::TopLevelModule::Yes) {
            process_custom_fetch_response->function()(*host->m_main_script_response, host->m_main_script_body);
            return {};
        }
        auto stored_script = host->m_imported_module_scripts.find_if([&](auto const& resource) { return resource.url == request->url(); });
        if (stored_script == host->m_imported_module_scripts.end()) {
            process_custom_fetch_response->function()(Web::Fetch::Infrastructure::ResponseView::network_error("The service worker did not record this module script"_string), Empty {});
            return {};
        }
        process_custom_fetch_response->function()(Web::ServiceWorker::response_from_script_resource(*stored_script), MUST(Core::ImmutableBytes::copy(stored_script->body)));
        return {};
    });
    Web::HTML::OnFetchScriptComplete on_complete = GC::create_function(GC::Heap::the(), GC::captures(NonnullRefPtr { *this }, page), [](auto& host, auto& page, GC::Ptr<Web::HTML::Script> script) -> void {
        if (!script) {
            as<PageHost>(page->client()).did_run_service_worker(false, {});
            return;
        }
        host->run_service_worker(page, *script, *host->m_main_script_response);
    });
    auto job_client = Web::HTML::RemoteEnvironmentSettings::create(m_request.script_fetch.job_client);
    if (Web::HTML::fetch_module_worker_script_graph(m_request.worker.script_url, job_client, Web::Fetch::Infrastructure::Request::Destination::ServiceWorker, *m_settings_object, perform_the_fetch_hook, on_complete).is_error())
        as<PageHost>(page->client()).did_run_service_worker(false, {});
}

// https://w3c.github.io/ServiceWorker/#run-service-worker-algorithm
void ServiceWorkerHost::run_service_worker(GC::Ref<Web::Page> page, GC::Ref<Web::HTML::Script> script, GC::Ref<Web::Fetch::Infrastructure::ResponseView> response)
{
    auto& host = as<PageHost>(page->client());

    // 1. If serviceWorker is running, then return serviceWorker's start status.
    // 2. If serviceWorker's state is "redundant", then return failure.
    // 3. Assert: serviceWorker's start status is null.
    // 4. Let script be serviceWorker's script resource.
    // 5. Assert: script is not null.
    // 6. Let startFailed be false.
    // 7. Let workerGlobalScope be serviceWorker's global object.
    // 8. If workerGlobalScope is null:
    //    1. Set workerGlobalScope to be the result of running the Setup ServiceWorkerGlobalScope algorithm with
    //       serviceWorker.
    //    2. If workerGlobalScope is null, then return failure.
    //    3. Set serviceWorker's global object to workerGlobalScope.
    // NB: The worker's process sets the global object up when it starts, and finishes the setup once the worker's
    //     script is fetched.
    if (!finish_setting_up_service_worker_global_scope(response)) {
        host.did_run_service_worker(false, {});
        return;
    }
    auto& worker_global_scope = *m_global_scope;

    // 9. Obtain agent for workerGlobalScope's realm execution context, and run the following steps in that context:
    // FIXME: 1. Set workerGlobalScope's force bypass cache for import scripts flag if forceBypassCache is true.
    // 2. If serviceWorker is an active worker, and there are any tasks queued in serviceWorker's containing service
    //    worker registration's task queues, queue them to serviceWorker's event loop's task queues in the same order
    //    using their original task sources.
    // NB: A new worker is not an active worker.

    // 3. Let evaluationStatus be null.
    // 4. If script is a classic script, then:
    //    1. Set evaluationStatus to the result of running the classic script script.
    //    2. If evaluationStatus.[[Value]] is empty, this means the script was not evaluated. Set startFailed to true and
    //       abort these steps.
    m_settings_object->execution_ready = true;
    JS::Completion evaluation_status;
    if (auto* classic_script = as_if<Web::HTML::ClassicScript>(*script)) {
        evaluation_status = classic_script->run();
    }
    // 5. Otherwise, if script is a module script, then:
    else {
        // 1. Let evaluationPromise be the result of running the module script script, with report errors set to false.
        auto evaluation_promise = as<Web::HTML::ModuleScript>(*script).run(Web::HTML::ModuleScript::PreventErrorReporting::Yes);

        // 2. Assert: evaluationPromise.[[PromiseState]] is not "pending".
        // NB: A null evaluationPromise is that of a script whose error to rethrow is not null.
        auto const* promise = evaluation_promise ? &as<JS::Promise>(*evaluation_promise->promise()) : nullptr;
        VERIFY(!promise || promise->state() != JS::Promise::State::Pending);

        // 3. If evaluationPromise.[[PromiseState]] is "rejected":
        if (!promise || promise->state() == JS::Promise::State::Rejected) {
            // 1. Set evaluationStatus to ThrowCompletion(evaluationPromise.[[PromiseResult]]).
            evaluation_status = JS::throw_completion(promise ? promise->result() : JS::js_undefined());
        }
        // 4. Otherwise:
        else {
            // 1. Set evaluationStatus to NormalCompletion(undefined).
            evaluation_status = JS::normal_completion(JS::js_undefined());
        }
    }

    // 6. If the script was aborted by the Terminate Service Worker algorithm, set startFailed to true and abort these
    //    steps.
    // 7. Set serviceWorker's start status to evaluationStatus.
    // NB: The UI process rejects the job of a worker whose start status is an abrupt completion.

    // 8. If script's has ever been evaluated flag is unset, then:
    //    1. For each eventType of settingsObject's global object's associated list of event listeners' event types:
    //       1. Append eventType to workerGlobalScope's associated service worker's set of event types to handle.
    //    2. Set script's has ever been evaluated flag.
    //    3. Unset the serviceWorker's all fetch listeners are empty flag.
    //    FIXME: 4. The user agent may, if the All Fetch Listeners Are Empty algorithm with workerGlobalScope returns
    //              true, set serviceWorker's all fetch listeners are empty flag.
    Vector<String> event_types_to_handle;
    for (auto const& event_type : worker_global_scope.event_types_of_event_listeners())
        event_types_to_handle.append(event_type.to_utf16_string().to_utf8());

    // 9. Run the responsible event loop specified by settingsObject until it is destroyed.
    m_settings_object->responsible_event_loop().schedule();

    // FIXME: 10. Clear workerGlobalScope's map of active timers.

    // 10. Wait for serviceWorker to be running, or for startFailed to be true.
    // 11. If startFailed is true, then return failure.
    // 12. Return serviceWorker's start status.
    // NB: The UI process skips the install event of a worker with no listener for it, so a new worker records its
    //     scripts once it has run. Scripts that the install event imports reach the UI process once it ends.
    if (!evaluation_status.is_abrupt() && !m_restarted_from_stored_scripts)
        record_scripts();
    m_is_running = !evaluation_status.is_abrupt();
    host.did_run_service_worker(m_is_running, move(event_types_to_handle));
    dispatch_waiting_fetch_events();
}

void ServiceWorkerHost::service_worker_state_changed(Web::ServiceWorker::ServiceWorkerDescriptor const& worker)
{
    if (m_global_scope)
        m_global_scope->service_worker_state_changed(worker);
    if (worker.id != m_request.worker.id)
        return;
    m_request.worker.state = worker.state;
    dispatch_waiting_fetch_events();
}

void ServiceWorkerHost::registration_state_changed(Web::ServiceWorker::ServiceWorkerRegistrationId registration_id, Web::ServiceWorker::ServiceWorkerRegistrationSlot target, Optional<Web::ServiceWorker::ServiceWorkerDescriptor> const& source)
{
    if (m_global_scope && registration_id == m_request.registration.id)
        m_global_scope->registration_state_changed(target, source);
}

// https://w3c.github.io/ServiceWorker/#service-worker-postmessage-options
void ServiceWorkerHost::dispatch_message_event(Web::ServiceWorker::ServiceWorkerMessageSource source, URL::Origin const& origin, Web::HTML::SerializedTransferRecord message, AK::Duration timeout)
{
    if (!m_global_scope)
        return;

    // 6.2. Queue a task on the DOM manipulation task source to run the following steps:
    auto record = make<Web::HTML::SerializedTransferRecord>(move(message));
    Web::HTML::queue_global_task(Web::HTML::Task::Source::DOMManipulation, Web::HTML::relevant_global_object(*m_global_scope), GC::create_function(GC::Heap::the(), GC::captures(NonnullRefPtr { *this }, move(source), origin, move(record), timeout), [](auto& protected_this, auto& source, auto& origin, auto& record, auto timeout) {
        auto event_id = Web::ServiceWorker::dispatch_message_event(*protected_this->m_global_scope, source, origin, move(*record), protected_this.ptr(), [](void* ctx, bool has_pending_events) {
            static_cast<ServiceWorkerHost*>(ctx)->pending_events_changed(has_pending_events);
        });
        if (event_id == 0)
            return;

        // NB: An extendable event's timed out flag "is set after an optional user agent imposed delay if the pending
        //     promises count is greater than zero", which this is.
        auto timer = Core::Timer::create_single_shot(timeout, [protected_this, event_id] {
            Web::ServiceWorker::time_out_extended_event(event_id);

            // NB: The timer outlives its own handler.
            Core::deferred_invoke([protected_this, event_id] {
                protected_this->m_extended_event_deadline_timers.remove(event_id);
            });
        });
        timer->start();
        protected_this->m_extended_event_deadline_timers.set(event_id, move(timer));
    }));
}

void ServiceWorkerHost::pending_events_changed(bool has_pending_events)
{
    as<PageHost>(m_page->client()).did_change_service_worker_pending_events(has_pending_events);
}

void ServiceWorkerHost::dispatch_lifecycle_event(Web::ServiceWorker::ServiceWorkerLifecycleEvent event, AK::Duration timeout)
{
    if (!m_global_scope || m_lifecycle_event_in_flight.has_value())
        return;
    m_lifecycle_event_in_flight = event;

    // https://w3c.github.io/ServiceWorker/#installation-algorithm
    // 11.3.1. Queue a task task on installingWorker's event loop using the DOM manipulation task source to run the
    //         following steps:
    // https://w3c.github.io/ServiceWorker/#activation-algorithm
    // 10.1.1. Queue a task task on activeWorker's event loop using the DOM manipulation task source to run the following
    //         steps:
    Web::HTML::queue_global_task(Web::HTML::Task::Source::DOMManipulation, Web::HTML::relevant_global_object(*m_global_scope), GC::create_function(GC::Heap::the(), GC::captures(NonnullRefPtr { *this }, event), [](auto& host, auto event) {
        auto& realm = host->m_settings_object->realm();
        web_rust_service_worker_dispatch_lifecycle_event(JS::HostABI::vm_to_abi(realm.vm()), &realm, host->m_global_scope.ptr(), to_underlying(event), host.ptr(), [](void* ctx, bool failed) {
            static_cast<ServiceWorkerHost*>(ctx)->lifecycle_event_finished(failed);
        });
    }));

    // NB: An extendable event's timed out flag "is set after an optional user agent imposed delay if the pending
    //     promises count is greater than zero", which this is.
    m_lifecycle_event_deadline_timer = Core::Timer::create_single_shot(timeout, [] {
        web_rust_service_worker_time_out_lifecycle_event(JS::HostABI::vm_to_abi(JS::VM::the()));
    });
    m_lifecycle_event_deadline_timer->start();
}

void ServiceWorkerHost::lifecycle_event_finished(bool failed)
{
    auto event = m_lifecycle_event_in_flight.release_value();
    if (m_lifecycle_event_deadline_timer)
        m_lifecycle_event_deadline_timer->stop();

    auto& host = as<PageHost>(m_page->client());
    if (event == Web::ServiceWorker::ServiceWorkerLifecycleEvent::Install && !failed) {
        m_global_scope->prune_script_resource_map();
        record_scripts();
    }
    host.did_finish_service_worker_lifecycle_event(event, !failed);
}

// NB: The UI process keeps the scripts of an installed worker, to run the worker again once it stops.
void ServiceWorkerHost::record_scripts()
{
    Vector<Web::ServiceWorker::ServiceWorkerScriptResource> scripts;
    scripts.append(Web::ServiceWorker::script_resource_from_response(m_request.worker.script_url, *m_main_script_response, m_main_script_body.bytes()));
    scripts.extend(m_global_scope->imported_script_resources());
    scripts.extend(m_imported_module_scripts);
    as<PageHost>(m_page->client()).did_record_service_worker_scripts(move(scripts));
}

void ServiceWorkerHost::skip_waiting_finished(Web::ServiceWorker::ServiceWorkerRequestId request_id)
{
    if (m_global_scope)
        m_global_scope->skip_waiting_finished(request_id);
}

void ServiceWorkerHost::connect_fetch_channel(IPC::TransportHandle handle, Web::HTML::EnvironmentId client)
{
    auto transport = handle.create_transport();
    if (transport.is_error()) {
        dbgln("Unable to take up a service worker fetch channel: {}", transport.error());
        return;
    }
    m_fetch_connections.append(ServiceWorkerFetchConnection::construct(transport.release_value(), *this, move(client)));
}

void ServiceWorkerHost::fetch_connection_closed(ServiceWorkerFetchConnection& connection)
{
    // NB: A client that went away reads none of the bodies of its fetch events.
    m_fetch_events.remove_all_matching([&](auto const& fetch_event) {
        if (fetch_event->connection.ptr() != &connection)
            return false;
        if (fetch_event->body_reader)
            Web::Fetch::Infrastructure::cancel_incremental_read(*fetch_event->body_reader);
        return true;
    });

    // NB: The connection is closing, so it is let go once it has.
    Core::deferred_invoke([protected_this = NonnullRefPtr { *this }, connection = NonnullRefPtr { connection }] {
        protected_this->m_fetch_connections.remove_all_matching([&](auto const& fetch_connection) { return fetch_connection == connection; });
    });
}

ServiceWorkerHost::FetchEventInFlight* ServiceWorkerHost::fetch_event_in_flight(ServiceWorkerFetchConnection const& connection, Web::ServiceWorker::FetchEventId fetch_id)
{
    for (auto& fetch_event : m_fetch_events) {
        if (fetch_event->connection.ptr() == &connection && fetch_event->fetch_id == fetch_id)
            return fetch_event.ptr();
    }
    return nullptr;
}

ServiceWorkerHost::FetchEventInFlight* ServiceWorkerHost::fetch_event_with_dispatch_id(u64 dispatch_id)
{
    for (auto& fetch_event : m_fetch_events) {
        if (fetch_event->dispatch_id == dispatch_id)
            return fetch_event.ptr();
    }
    return nullptr;
}

void ServiceWorkerHost::forget_fetch_event(FetchEventInFlight const& fetch_event)
{
    m_fetch_events.remove_first_matching([&](auto const& in_flight) { return in_flight.ptr() == &fetch_event; });
}

// https://w3c.github.io/ServiceWorker/#create-fetch-event-and-dispatch
bool ServiceWorkerHost::can_dispatch_fetch_events() const
{
    // 15. If activeWorker's state is "activating", wait for activeWorker's state to become "activated".
    // 16. If the result of running the Run Service Worker algorithm with activeWorker is failure, then set
    //     handleFetchFailed to true.
    // NB: The UI process runs the worker, which runs once its script has been evaluated.
    return m_is_running && m_request.worker.state == Web::ServiceWorker::ServiceWorkerState::Activated;
}

void ServiceWorkerHost::dispatch_fetch_event(ServiceWorkerFetchConnection& connection, Web::ServiceWorker::FetchEventId fetch_id, Web::ServiceWorker::FetchEventRequest request)
{
    if (fetch_event_in_flight(connection, fetch_id)) {
        connection.did_misbehave("A client sent a fetch event twice");
        return;
    }
    auto fetch_event = make<FetchEventInFlight>();
    fetch_event->connection = connection.make_weak_ptr<ServiceWorkerFetchConnection>();
    fetch_event->fetch_id = fetch_id;
    fetch_event->dispatch_id = ++m_next_fetch_dispatch_id;
    fetch_event->request_waiting_for_dispatch = move(request);
    auto& in_flight = *fetch_event;
    m_fetch_events.append(move(fetch_event));
    if (can_dispatch_fetch_events())
        run_fetch_event_task(in_flight);
}

void ServiceWorkerHost::dispatch_waiting_fetch_events()
{
    // https://w3c.github.io/ServiceWorker/#create-fetch-event-and-dispatch
    // 16. If the result of running the Run Service Worker algorithm with activeWorker is failure, then set
    //     handleFetchFailed to true.
    // 22.2. Return networkError.
    if ((m_settings_object && !m_is_running && !m_lifecycle_event_in_flight.has_value() && m_request.worker.state != Web::ServiceWorker::ServiceWorkerState::Parsed)
        || m_request.worker.state == Web::ServiceWorker::ServiceWorkerState::Redundant) {
        m_fetch_events.remove_all_matching([](auto const& fetch_event) {
            if (!fetch_event->request_waiting_for_dispatch.has_value())
                return false;
            if (auto connection = fetch_event->connection.strong_ref())
                connection->async_fetch_event_settled(fetch_event->fetch_id, Web::ServiceWorker::FetchEventOutcome::NetworkError, {});
            return true;
        });
        return;
    }
    if (!can_dispatch_fetch_events())
        return;
    for (auto& fetch_event : m_fetch_events) {
        if (fetch_event->request_waiting_for_dispatch.has_value())
            run_fetch_event_task(*fetch_event);
    }
}

// https://w3c.github.io/ServiceWorker/#create-fetch-event-and-dispatch
// 17.3.1. Let requestObject be the result of creating a Request object, given request, [...]
// NB: What the client's process says of request.
static void fetch_event_settled(void* ctx, u8 outcome, void* response);

void ServiceWorkerHost::run_fetch_event_task(FetchEventInFlight& fetch_event)
{
    auto descriptor = fetch_event.request_waiting_for_dispatch.release_value();
    auto connection = fetch_event.connection.strong_ref();
    if (!connection) {
        forget_fetch_event(fetch_event);
        return;
    }

    // https://w3c.github.io/ServiceWorker/#create-fetch-event-and-dispatch
    // 17.4.9. If request is a non-subresource request, request's destination is not "report", and reservedClient is
    //         not null, initialize e's resultingClientId attribute to reservedClient's id.
    // NB: The channel carries the fetch events of its client: the reserved client of a non-subresource request, and the
    //     client of a subresource request.
    auto client_id = descriptor.client_id.value_or({});
    auto replaces_client_id = move(descriptor.replaces_client_id);
    auto navigation_preload_header_value = move(descriptor.navigation_preload_header_value);
    auto request = Web::ServiceWorker::request_from(move(descriptor));
    String resulting_client_id;
    if (request->is_non_subresource_request() && request->destination() != Web::Fetch::Infrastructure::Request::Destination::Report)
        resulting_client_id = connection->client().to_string();
    else
        client_id = connection->client().to_string();

    // 17.4. Queue a task task to run the following substeps:
    //       The task must use activeWorker's event loop and the handle fetch task source.
    auto dispatch_id = fetch_event.dispatch_id;
    // https://w3c.github.io/ServiceWorker/#on-fetch-request-algorithm
    // 15.1. If registration's navigation preload enabled flag is set then:
    GC::Ptr<Web::WebIDL::Promise> preload_response;
    if (navigation_preload_header_value.has_value())
        preload_response = Web::ServiceWorker::start_navigation_preload(*m_settings_object, *request, *navigation_preload_header_value);

    Web::HTML::queue_global_task(Web::HTML::Task::Source::HandleFetch, Web::HTML::relevant_global_object(*m_global_scope), GC::create_function(GC::Heap::the(), GC::captures(NonnullRefPtr { *this }, dispatch_id, request, move(client_id), move(resulting_client_id), move(replaces_client_id), preload_response), [](auto& protected_this, auto& dispatch_id, auto& request, auto& client_id, auto& resulting_client_id, auto& replaces_client_id, auto& preload_response) {
        if (!protected_this->fetch_event_with_dispatch_id(dispatch_id))
            return;
        auto& realm = protected_this->m_settings_object->realm();

        // 2. Let abortController be a new AbortController object with workerRealm.
        auto abort_controller = Web::DOM::AbortController::create();

        // 3. Let requestObject be the result of creating a Request object, given request, a new Headers object's guard
        //    which is "immutable", abortController's signal, and workerRealm.
        auto request_object = Web::Fetch::Request::create(request, Web::Fetch::Headers::Guard::Immutable, abort_controller->signal());
        request_object->create_body_over_request_body_bytes(*protected_this->m_settings_object);
        auto request_wrapper = Web::Bindings::wrap(Web::Bindings::host_defined_wrapper_world(realm), realm, request_object);

        // 1. Let e be the result of creating an event with FetchEvent.
        // [...]
        // 14. Invoke Update Service Worker Extended Events Set with activeWorker and e.
        // NB: The service worker's Rust bindings run these substeps, and the steps after the task.
        auto extended_event_id = web_rust_service_worker_dispatch_fetch_event(
            JS::HostABI::vm_to_abi(realm.vm()), &realm, protected_this->m_global_scope.ptr(), JS::HostABI::object_to_abi(request_wrapper.ptr()),
            client_id.bytes(), resulting_client_id.bytes(), replaces_client_id.bytes(), preload_response.ptr(),
            bit_cast<void*>(static_cast<FlatPtr>(dispatch_id)), fetch_event_settled,
            protected_this.ptr(), [](void* ctx, bool has_pending_events) {
                static_cast<ServiceWorkerHost*>(ctx)->pending_events_changed(has_pending_events);
            });
        if (extended_event_id == 0)
            return;

        // NB: An extendable event's timed out flag "is set after an optional user agent imposed delay if the pending
        //     promises count is greater than zero", which this is.
        auto timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(protected_this->m_request.extendable_event_timeout_ms), [protected_this, extended_event_id] {
            Web::ServiceWorker::time_out_extended_event(extended_event_id);

            // NB: The timer outlives its own handler.
            Core::deferred_invoke([protected_this, extended_event_id] {
                protected_this->m_extended_event_deadline_timers.remove(extended_event_id);
            });
        });
        timer->start();
        protected_this->m_extended_event_deadline_timers.set(extended_event_id, move(timer));
    }));
}

static void fetch_event_settled(void* ctx, u8 outcome, void* response)
{
    auto dispatch_id = static_cast<u64>(bit_cast<FlatPtr>(ctx));
    if (auto host = fetch_event_host().strong_ref())
        host->fetch_event_did_settle(dispatch_id, static_cast<Web::ServiceWorker::FetchEventOutcome>(outcome), static_cast<JS::Object*>(response));
}

// https://fetch.spec.whatwg.org/#concept-response
// NB: What the client's process makes potentialResponse from: its type, and the fields of its internal response.
void ServiceWorkerHost::fetch_event_did_settle(u64 dispatch_id, Web::ServiceWorker::FetchEventOutcome outcome, JS::Object* response_wrapper)
{
    auto* fetch_event = fetch_event_with_dispatch_id(dispatch_id);
    if (!fetch_event)
        return;
    auto connection = fetch_event->connection.strong_ref();
    if (!connection) {
        forget_fetch_event(*fetch_event);
        return;
    }

    auto* response_object = response_wrapper ? Web::Bindings::impl_from<Web::Fetch::Response>(response_wrapper) : nullptr;

    // AD-HOC: A response whose body was already read or locked gives nothing to read, so the fetch gets a network
    //         error, as in other engines.
    if (outcome == Web::ServiceWorker::FetchEventOutcome::Response && (!response_object || response_object->is_unusable()))
        outcome = Web::ServiceWorker::FetchEventOutcome::NetworkError;
    if (outcome != Web::ServiceWorker::FetchEventOutcome::Response) {
        connection->async_fetch_event_settled(fetch_event->fetch_id, outcome, {});
        forget_fetch_event(*fetch_event);
        return;
    }

    auto response = response_object->response();
    auto descriptor = Web::ServiceWorker::describe_response(*response);
    connection->async_fetch_event_settled(fetch_event->fetch_id, outcome, descriptor);
    if (!descriptor.has_body) {
        forget_fetch_event(*fetch_event);
        return;
    }

    // https://w3c.github.io/ServiceWorker/#fetch-event-respondwith
    // 10.2.5.1. Let reader be the result of getting a reader from response's body's stream.
    // 10.2.5.8. Run these subsubsteps repeatedly in parallel while done is false:
    // NB: The worker reads the body here, and delivers its bytes to the client's process as far as that process allows.
    //     It reads the body of an opaque filtered response's internal response, which the client needs.
    auto& realm = m_settings_object->realm();
    auto on_chunk = GC::create_function(GC::Heap::the(), GC::captures(NonnullRefPtr { *this }, dispatch_id), [](auto& protected_this, auto& dispatch_id, ByteBuffer bytes) {
        auto* fetch_event = protected_this->fetch_event_with_dispatch_id(dispatch_id);
        if (!fetch_event)
            return;
        fetch_event->unsent_body_bytes.append(bytes);
        protected_this->send_response_body_bytes(*fetch_event);
    });
    auto on_end = GC::create_function(GC::Heap::the(), GC::captures(NonnullRefPtr { *this }, dispatch_id), [](auto& protected_this, auto& dispatch_id) {
        auto* fetch_event = protected_this->fetch_event_with_dispatch_id(dispatch_id);
        if (!fetch_event)
            return;
        fetch_event->body_end = true;
        protected_this->send_response_body_bytes(*fetch_event);
    });
    auto on_error = GC::create_function(GC::Heap::the(), GC::captures(NonnullRefPtr { *this }, dispatch_id), [](auto& protected_this, auto& dispatch_id, JS::Value) {
        auto* fetch_event = protected_this->fetch_event_with_dispatch_id(dispatch_id);
        if (!fetch_event)
            return;
        fetch_event->body_end = false;
        protected_this->send_response_body_bytes(*fetch_event);
    });
    fetch_event->body_reader = GC::make_root(response->unsafe_response()->body()->incrementally_read(realm, on_chunk, on_end, on_error, GC::Ref { realm.global_object() }));
}

void ServiceWorkerHost::send_response_body_bytes(FetchEventInFlight& fetch_event)
{
    auto connection = fetch_event.connection.strong_ref();
    if (!connection) {
        if (fetch_event.body_reader)
            Web::Fetch::Infrastructure::cancel_incremental_read(*fetch_event.body_reader);
        forget_fetch_event(fetch_event);
        return;
    }

    if (!fetch_event.unsent_body_bytes.is_empty() && fetch_event.sent_byte_count < fetch_event.allowed_byte_count) {
        auto byte_count = min(fetch_event.unsent_body_bytes.size(), fetch_event.allowed_byte_count - fetch_event.sent_byte_count);
        connection->async_fetch_response_body_data(fetch_event.fetch_id, fetch_event.unsent_body_bytes.bytes().trim(byte_count));
        fetch_event.unsent_body_bytes = MUST(ByteBuffer::copy(fetch_event.unsent_body_bytes.bytes().slice(byte_count)));
        fetch_event.sent_byte_count += byte_count;
    }

    // NB: An errored body ends at once. One that closed ends once the client has all of its bytes.
    if (fetch_event.body_end == false || (fetch_event.body_end == true && fetch_event.unsent_body_bytes.is_empty())) {
        connection->async_fetch_response_body_end(fetch_event.fetch_id, *fetch_event.body_end);
        forget_fetch_event(fetch_event);
    }
}

void ServiceWorkerHost::allow_fetch_response_body(ServiceWorkerFetchConnection& connection, Web::ServiceWorker::FetchEventId fetch_id, u64 total_byte_count)
{
    auto* fetch_event = fetch_event_in_flight(connection, fetch_id);
    if (!fetch_event)
        return;
    fetch_event->allowed_byte_count = max(fetch_event->allowed_byte_count, total_byte_count);
    send_response_body_bytes(*fetch_event);
}

void ServiceWorkerHost::cancel_fetch(ServiceWorkerFetchConnection& connection, Web::ServiceWorker::FetchEventId fetch_id)
{
    auto* fetch_event = fetch_event_in_flight(connection, fetch_id);
    if (!fetch_event)
        return;
    // FIXME: Signal abort on the fetch event's request's abortController.
    if (fetch_event->body_reader)
        Web::Fetch::Infrastructure::cancel_incremental_read(*fetch_event->body_reader);
    forget_fetch_event(*fetch_event);
}

}
