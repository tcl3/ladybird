/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibGC/Heap.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/ConsoleObject.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/ServiceWorkerGlobalScope.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/FileAPI/BlobURLStore.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/RemoteEnvironmentSettings.h>
#include <LibWeb/HTML/Scripting/WorkerEnvironmentSettingsObject.h>
#include <LibWeb/HTML/WorkerDebugConsoleClient.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/ServiceWorker/ServiceWorkerGlobalScope.h>
#include <LibWeb/ServiceWorker/Update.h>
#include <WebContent/Worker/PageHost.h>
#include <WebContent/Worker/ServiceWorkerHost.h>

extern "C" {
void web_rust_service_worker_dispatch_lifecycle_event(JSVM*, void* realm, Web::DOM::EventTarget* global, u8 kind, void* ctx, void (*on_finished)(void*, bool failed));
void web_rust_service_worker_time_out_lifecycle_event(JSVM*);
}

namespace WebWorker {

ServiceWorkerHost::ServiceWorkerHost(Web::ServiceWorker::ServiceWorkerStartRequest request)
    : m_request(move(request))
{
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
    };
    Web::ServiceWorker::fetch_the_script_of_a_new_service_worker(job_client, *m_settings_object, move(job), GC::create_function(GC::Heap::the(), GC::captures(NonnullRefPtr { *this }, page), [](auto& host, auto& page, GC::Ptr<Web::HTML::Script> script, GC::Ptr<Web::Fetch::Infrastructure::ResponseView> response, Core::ImmutableBytes body, Optional<Web::ServiceWorker::ServiceWorkerJobError> rejection) {
        if (!script || !response) {
            as<PageHost>(page->client()).did_fetch_service_worker_script(rejection.value_or(Web::ServiceWorker::ServiceWorkerJobError::TypeError));
            return;
        }
        host->m_main_script_response = response;
        host->m_main_script_body = move(body);
        host->run_service_worker(page, *script, *response);
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

    m_global_scope->restore_imported_script_resources(imported_scripts);
    auto response = Web::ServiceWorker::response_from_script_resource(*main_script);
    auto source_text = Utf16String::from_utf8_with_replacement_character(StringView { main_script->body.bytes() });
    auto script = Web::HTML::ClassicScript::create(main_script->url.to_byte_string(), source_text, *m_settings_object, main_script->url);
    m_main_script_response = response;
    m_main_script_body = Core::ImmutableBytes::adopt(move(main_script->body));
    run_service_worker(page, script, response);
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
    // 5. Otherwise, if script is a module script, then:
    // FIXME: Run module scripts.
    auto* classic_script = as_if<Web::HTML::ClassicScript>(*script);
    if (!classic_script) {
        host.did_run_service_worker(false, {});
        return;
    }
    m_settings_object->execution_ready = true;
    auto evaluation_status = classic_script->run();

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
    host.did_run_service_worker(!evaluation_status.is_abrupt(), move(event_types_to_handle));
}

void ServiceWorkerHost::service_worker_state_changed(Web::ServiceWorker::ServiceWorkerDescriptor const& worker)
{
    if (m_global_scope)
        m_global_scope->service_worker_state_changed(worker);
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
    as<PageHost>(m_page->client()).did_record_service_worker_scripts(move(scripts));
}

void ServiceWorkerHost::skip_waiting_finished(Web::ServiceWorker::ServiceWorkerRequestId request_id)
{
    if (m_global_scope)
        m_global_scope->skip_waiting_finished(request_id);
}

}
