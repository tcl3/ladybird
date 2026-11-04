/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/SourceTextModule.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/MIME.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/ResponseView.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/Fetching.h>
#include <LibWeb/HTML/Scripting/ModuleMap.h>
#include <LibWeb/HTML/Scripting/ModuleScript.h>
#include <LibWeb/HTML/Scripting/Script.h>
#include <LibWeb/ServiceWorker/ServiceWorkerGlobalScope.h>
#include <LibWeb/ServiceWorker/Update.h>

namespace Web::ServiceWorker {

// The consume-body steps of one of a job's script fetches. They run once: when the fetch consumes its body, or when it
// is stopped.
class ScriptFetchConsumeBody : public JS::Cell {
    GC_CELL(ScriptFetchConsumeBody, JS::Cell);
    GC_DECLARE_ALLOCATOR(ScriptFetchConsumeBody);

public:
    using Steps = GC::Function<Fetch::Infrastructure::FetchAlgorithms::ProcessResponseConsumeBodyFunction::FunctionType>;

    static GC::Ref<ScriptFetchConsumeBody> create(GC::Ref<Steps> steps)
    {
        return GC::Heap::the().allocate<ScriptFetchConsumeBody>(steps);
    }

    void run(GC::Ref<Fetch::Infrastructure::ResponseView> response, Fetch::Infrastructure::FetchAlgorithms::BodyBytes body_bytes)
    {
        if (auto steps = exchange(m_steps, nullptr))
            steps->function()(response, move(body_bytes));
    }

private:
    explicit ScriptFetchConsumeBody(GC::Ref<Steps> steps)
        : m_steps(steps)
    {
    }

    virtual void visit_edges(JS::Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_steps);
    }

    GC::Ptr<Steps> m_steps;
};

GC_DEFINE_ALLOCATOR(ScriptFetchConsumeBody);

class UpdateScriptFetchState : public RefCounted<UpdateScriptFetchState> {
public:
    void visit_edges(GC::Cell::Visitor& visitor) const
    {
        visitor.visit(m_response);
    }

    void reject_job_promise(ServiceWorkerJobError error)
    {
        if (!m_rejection.has_value())
            m_rejection = error;
    }

    Optional<ServiceWorkerJobError> rejection() const { return m_rejection; }

    GC::Ptr<Fetch::Infrastructure::ResponseView> response() const { return m_response; }
    void set_response(GC::Ref<Fetch::Infrastructure::ResponseView> response) { m_response = response; }

    Core::ImmutableBytes const& body() const { return m_body; }
    void set_body(Core::ImmutableBytes body) { m_body = move(body); }

    Vector<ServiceWorkerScriptResource> take_imported_module_scripts() { return move(m_imported_module_scripts); }
    void record_imported_module_script(ServiceWorkerScriptResource resource)
    {
        auto recorded = m_imported_module_scripts.find_if([&](auto const& recorded) { return recorded.url == resource.url; });
        if (recorded == m_imported_module_scripts.end())
            m_imported_module_scripts.append(move(resource));
    }

private:
    Optional<ServiceWorkerJobError> m_rejection;
    GC::Ptr<Fetch::Infrastructure::ResponseView> m_response;
    Core::ImmutableBytes m_body;
    Vector<ServiceWorkerScriptResource> m_imported_module_scripts;
};

// NB: The result of the processResponse steps of a job's top-level script fetch, which the perform the fetch hook
//     waits for.
struct ProcessResponseCompletionResult : RefCounted<ProcessResponseCompletionResult> {
    void visit_edges(GC::Cell::Visitor& visitor) const
    {
        if (value.has_value())
            value->visit_edges(visitor);
    }

    Optional<WebIDL::ExceptionOr<void>> value;
};

// https://w3c.github.io/ServiceWorker/#is-async-module
// AD-HOC: The algorithm takes a module script, whose record and base URL are the algorithm's record and base. Each
//         requested module's specifier is resolved against its own referring script, against which it was resolved
//         when the module was fetched.
static bool is_async_module(HTML::ModuleScript& script, HTML::ModuleMap const& module_map, Vector<URL::URL>& seen)
{
    // 1. If record is not a Cyclic Module Record, then:
    auto record = script.record();
    auto const* cyclic_module = record.get_pointer<GC::Ref<JS::SourceTextModule>>();
    if (!cyclic_module) {
        // 1. Return false.
        return false;
    }

    // 2. If record.[[Async]] is true, then:
    if ((*cyclic_module)->has_top_level_await()) {
        // 1. Return true.
        return true;
    }

    // 3. For each string requested of record.[[RequestedModules]]:
    for (auto const& requested : (*cyclic_module)->requested_modules()) {
        // 1. Let url be the result of resolving a module specifier given base and requested.
        // 2. Assert: url is never failure, because resolving a module specifier must have been previously successful
        //    with these same two arguments.
        auto url = MUST(HTML::resolve_module_specifier(script, requested.module_specifier));

        // 3. If seen does not contain url, then:
        if (seen.contains_slow(url))
            continue;

        // 1. Append url to seen.
        seen.append(url);

        // 2. If moduleMap[url] does not have a record, then:
        auto entry = module_map.get(url, HTML::module_type_from_module_request(requested));
        auto const* module_script = entry.has_value() ? entry->get_pointer<GC::Ref<HTML::ModuleScript>>() : nullptr;
        if (!module_script || (*module_script)->record().has<Empty>()) {
            // 1. Return false.
            return false;
        }

        // 3. If Is Async Module for moduleMap[url]'s record, moduleMap, base, and seen is true, then:
        if (is_async_module(**module_script, module_map, seen)) {
            // 1. Return true.
            return true;
        }
    }

    // 4. Return false.
    return false;
}

static GC::Ref<Fetch::Infrastructure::FetchController> fetch_job_script(HTML::EnvironmentSettingsObject& settings_object, Fetch::Infrastructure::Request& request, Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input, GC::Ref<ScriptFetchConsumeBody> consume_body)
{
    fetch_algorithms_input.process_response_consume_body = GC::create_function(GC::Heap::the(), GC::captures(consume_body), [](auto& consume_body, GC::Ref<Fetch::Infrastructure::ResponseView> response, Fetch::Infrastructure::FetchAlgorithms::BodyBytes body_bytes) {
        consume_body->run(response, move(body_bytes));
    });

    auto& realm = settings_object.realm();
    auto fetch_controller = Fetch::Fetching::fetch(realm, request, Fetch::Infrastructure::FetchAlgorithms::create(move(fetch_algorithms_input)));

    // AD-HOC: Stopping a fetch does not guarantee that its consume-body steps run, so the stop completes the script
    //         fetch as with a network error.
    fetch_controller->set_stop_steps(GC::create_function(realm.heap(), GC::captures(consume_body), [](auto& consume_body) {
        consume_body->run(Fetch::Infrastructure::ResponseView::network_error("Service Worker script fetch was stopped"_string), Empty {});
    }));
    return fetch_controller;
}

// https://w3c.github.io/ServiceWorker/#update-algorithm
void fetch_the_script_of_a_new_service_worker(HTML::FetchClient const& job_client, HTML::EnvironmentSettingsObject& settings_object, UpdateScriptFetch job, OnUpdateScriptFetchComplete on_complete)
{
    // NB: The UI process runs steps 1 to 4, and the steps from 8 on with what this reports.

    // 5. Let hasUpdatedResources be false.
    // 6. Let updatedResourceMap be an ordered map where the keys are URLs and the values are responses.
    // NB: The worker's process compares the script with the newest worker's once it is fetched, and keeps the script
    //     resource map.
    auto state = adopt_ref(*new UpdateScriptFetchState);

    // To perform the fetch hook given request, run the following steps:
    HTML::PerformTheFetchHook perform_the_fetch_hook = GC::create_function(GC::Heap::the(), GC::captures(GC::Ref { settings_object }, job, state), [](auto& settings_object, auto& job, auto& state, NonnullRefPtr<Fetch::Infrastructure::Request> request, HTML::TopLevelModule top_level, HTML::ProcessCustomFetchResponse process_custom_fetch_response) -> WebIDL::ExceptionOr<void> {
        // 1. Append `Service-Worker`/`script` to request's header list.
        // NOTE: See the definition of the Service-Worker header in Appendix B: Extended HTTP headers.
        request->header_list()->append(HTTP::Header::isomorphic_encode("Service-Worker"sv, "script"sv));

        // 2. Set request's cache mode to "no-cache" if any of the following are true:
        //    - registration's update via cache mode is not "all".
        //    - job's force bypass cache flag is set.
        //    - newestWorker is not null and registration is stale.
        if (job.update_via_cache != ServiceWorkerUpdateViaCache::All || job.force_bypass_cache || job.newest_worker_is_stale)
            request->set_cache_mode(HTTP::CacheMode::NoCache);

        // 3. Set request's service-workers mode to "none".
        request->set_service_workers_mode(Fetch::Infrastructure::Request::ServiceWorkersMode::None);

        Web::Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input { Web::Fetch::Engine::BodyIntent::ConsumeForCallback };
        auto consume_body = ScriptFetchConsumeBody::create(GC::create_function(GC::Heap::the(), GC::captures(state, top_level, request->url(), process_custom_fetch_response), [](auto& state, auto& top_level, auto& url, auto& process_custom_fetch_response, GC::Ref<Fetch::Infrastructure::ResponseView> response, Fetch::Infrastructure::FetchAlgorithms::BodyBytes body_bytes) {
            if (auto const* body = body_bytes.get_pointer<Core::ImmutableBytes>()) {
                if (top_level == HTML::TopLevelModule::Yes) {
                    state->set_body(*body);
                } else if (!response->is_network_error()) {
                    // AD-HOC: The module scripts that a module worker's script imports go into the worker's script
                    //         resource map, against which an update checks them byte for byte, as in other engines.
                    //         The worker's process restarts the worker from them.
                    state->record_imported_module_script(script_resource_from_response(url, response, body->bytes()));
                }
            }
            process_custom_fetch_response->function()(response, move(body_bytes));
        }));

        // 4. If the isTopLevel flag is unset, then return the result of fetching request.
        // FIXME: Needs spec issue, this wording is confusing and contradicts the way perform the fetch hook is used in `run a worker`
        if (top_level == HTML::TopLevelModule::No) {
            (void)fetch_job_script(*settings_object, *request, move(fetch_algorithms_input), consume_body);
            return {};
        }

        // 5. Set request's redirect mode to "error".
        // AD-HOC: The redirect mode is "manual", so that a redirect can be told apart from other network errors.
        request->set_redirect_mode(Fetch::Infrastructure::Request::RedirectMode::Manual);

        // 6. Fetch request, and asynchronously wait to run the remaining steps as part of fetch's processResponse for the response response.
        // Note: The rest of the steps are in the processCustomFetchResponse algorithm
        // FIXME: Needs spec issue to mention the existence of processCustomFetchResponse, same as step 4

        // FIXME: Is there a better way to 'wait' for the fetch's processResponse to complete?
        //        Is this actually what the spec wants us to do?
        auto process_response_completion_result = make_ref_counted<ProcessResponseCompletionResult>();

        fetch_algorithms_input.process_response = GC::create_function(GC::Heap::the(), GC::captures(job, state, consume_body, process_response_completion_result), [](auto& job, auto& state, auto& consume_body, auto& process_response_completion_result, GC::Ref<Fetch::Infrastructure::ResponseView> response) -> void {
            // AD-HOC: A redirect, a network error or a response whose status is not ok completes these steps as a network
            //         error. A redirect rejects the job with a "SecurityError" DOMException, and the others with a
            //         TypeError rather than as a response of the wrong MIME type, matching other engines.
            if (response->type() == Fetch::Infrastructure::Response::Type::OpaqueRedirect)
                state->reject_job_promise(ServiceWorkerJobError::SecurityError);
            if (response->is_network_error() || !Fetch::Infrastructure::is_ok_status(response->status())) {
                consume_body->run(Fetch::Infrastructure::ResponseView::network_error("Service Worker script fetch failed"_string), Empty {});
                process_response_completion_result->value = WebIDL::ExceptionOr<void> {};
                return;
            }

            // 7. Extract a MIME type from the response's header list. If this MIME type (ignoring parameters) is not a JavaScript MIME type, then:
            auto mime_type = Fetch::Infrastructure::extract_mime_type(response->header_list());
            if (!mime_type.has_value() || !mime_type->is_javascript()) {
                // 1. Invoke Reject Job Promise with job and "SecurityError" DOMException.
                state->reject_job_promise(ServiceWorkerJobError::SecurityError);

                // 2. Asynchronously complete these steps with a network error.
                consume_body->run(Fetch::Infrastructure::ResponseView::network_error("Service Worker script response is not a JavaScript MIME type"_string), Empty {});
                process_response_completion_result->value = WebIDL::ExceptionOr<void> {};
                return;
            }

            // 8. Let serviceWorkerAllowed be the result of get `Service-Worker-Allowed` from response's header list.
            // NOTE: See the definition of the Service-Worker-Allowed header in Appendix B: Extended HTTP headers.
            auto service_worker_allowed_bytes = response->header_list()->get("Service-Worker-Allowed"sv);

            // 9. If serviceWorkerAllowed is not null, then set serviceWorkerAllowed to the result of isomorphic decode serviceWorkerAllowed.
            Optional<Utf16String> service_worker_allowed;
            if (service_worker_allowed_bytes.has_value())
                service_worker_allowed = TextCodec::isomorphic_decode_to_utf16(*service_worker_allowed_bytes);

            // 10. Set policyContainer to the result of creating a policy container from a fetch response given response.
            // NB: Setup ServiceWorkerGlobalScope creates the worker global scope's policy container from the response.

            // 11. Let scopeURL be registration's scope url.
            auto const& scope_url = job.scope_url;

            // 12. Let maxScopeString be null.
            auto max_scope_string = Optional<ByteString> {};

            auto join_paths_with_slash = [](URL::URL const& url) -> ByteString {
                StringBuilder builder;
                builder.append('/');
                builder.join('/', url.path_segments());
                return builder.to_byte_string();
            };

            // 13. If serviceWorkerAllowed is null, then:
            if (!service_worker_allowed.has_value()) {
                // 1. Let resolvedScope be the result of parsing "./" using job's script url as the base URL.
                auto resolved_scope = DOMURL::parse({ u"./", 2 }, job.script_url);

                // 2. Set maxScopeString to "/", followed by the strings in resolvedScope's path (including empty strings), separated from each other by "/".
                max_scope_string = join_paths_with_slash(*resolved_scope);
            }
            // 14. Else:
            else {
                // 1. Let maxScope be the result of parsing serviceWorkerAllowed using job's script url as the base URL.
                auto max_scope = DOMURL::parse(*service_worker_allowed, job.script_url);

                // 2. If maxScope is failure, then:
                if (!max_scope.has_value()) {
                    // 1. Asynchronously complete these steps with a network error.
                    consume_body->run(Fetch::Infrastructure::ResponseView::network_error("Failed to parse Service-Worker-Allowed header as a URL"_string), Empty {});
                    process_response_completion_result->value = WebIDL::ExceptionOr<void> {};
                    return;
                }

                // 3. If maxScope's origin is job's script url's origin, then:
                if (max_scope->origin().is_same_origin(job.script_url.origin())) {
                    // 1. Set maxScopeString to "/", followed by the strings in maxScope's path (including empty strings), separated from each other by "/".
                    max_scope_string = join_paths_with_slash(*max_scope);
                }
            }

            // 15. Let scopeString be "/", followed by the strings in scopeURL's path (including empty strings), separated from each other by "/".
            auto scope_string = join_paths_with_slash(scope_url);

            // 16. If maxScopeString is null or scopeString does not start with maxScopeString, then:
            if (!max_scope_string.has_value() || !scope_string.starts_with(max_scope_string.value())) {
                // 1. Invoke Reject Job Promise with job and "SecurityError" DOMException.
                state->reject_job_promise(ServiceWorkerJobError::SecurityError);

                // 2. Asynchronously complete these steps with a network error.
                consume_body->run(Fetch::Infrastructure::ResponseView::network_error("Service Worker script scope does not match Service-Worker-Allowed header"_string), Empty {});
                process_response_completion_result->value = WebIDL::ExceptionOr<void> {};
                return;
            }

            // 17. Let url be request's url.
            // 18. Set updatedResourceMap[url] to response.
            state->set_response(response);
            // FIXME: 19. If response's cache state is not "local", set registration's last update check time to the current time.
            // 20. Set hasUpdatedResources to true if any of the following are true:
            // 21. If hasUpdatedResources is false and newestWorker's classic scripts imported flag is set, then:
            // NB: The worker's process compares the scripts once the body is read, before it runs the script.

            // 22. Asynchronously complete these steps with response.
            process_response_completion_result->value = WebIDL::ExceptionOr<void> {};
        });

        auto fetch_controller = fetch_job_script(*settings_object, *request, move(fetch_algorithms_input), consume_body);

        // FIXME: This feels.. uncomfortable but it should work to block the current task until the fetch has progressed past our processResponse hook or aborted
        settings_object->responsible_event_loop().spin_until(GC::create_function(GC::Heap::the(), GC::captures(fetch_controller, process_response_completion_result), [](auto& fetch_controller, auto& process_response_completion_result) -> bool {
            if (process_response_completion_result->value.has_value())
                return true;
            if (fetch_controller->state() == Fetch::Infrastructure::FetchController::State::Terminated || fetch_controller->state() == Fetch::Infrastructure::FetchController::State::Aborted) {
                process_response_completion_result->value = WebIDL::AbortError::create("Service Worker fetch was terminated or aborted"_utf16);
                return true;
            }
            // NB: A stopped fetch's stop steps have completed the script fetch.
            if (fetch_controller->state() == Fetch::Infrastructure::FetchController::State::Stopped) {
                process_response_completion_result->value = WebIDL::ExceptionOr<void> {};
                return true;
            }
            return false;
        }));

        // NB: The event loop gave up on an aborted script.
        if (!process_response_completion_result->value.has_value())
            return WebIDL::AbortError::create("Service Worker fetch was terminated or aborted"_utf16);
        return process_response_completion_result->value.release_value();
    });

    // When the algorithm asynchronously completes, continue the rest of these steps, with script being the asynchronous completion value.
    HTML::OnFetchScriptComplete on_fetch_complete = GC::create_function(GC::Heap::the(), GC::captures(state, GC::Ref { settings_object }, on_complete), [](auto& state, auto& settings_object, auto& on_complete, GC::Ptr<HTML::Script> script) -> void {
        // 8. If script is null or Is Async Module with script's record, script's base URL, and « » is true, then:
        //    1. Invoke Reject Job Promise with job and TypeError.
        // NB: The UI process runs the rest of the substeps for a null script.
        if (auto* module_script = as_if<HTML::ModuleScript>(script.ptr())) {
            Vector<URL::URL> seen;
            if (is_async_module(*module_script, settings_object->module_map(), seen)) {
                state->reject_job_promise(ServiceWorkerJobError::TypeError);
                script = nullptr;
            }
        }
        on_complete->function()(script, state->response(), state->body(), state->take_imported_module_scripts(), state->rejection());
    });

    // 7. Switching on job's worker type, run these substeps with the following options:
    switch (job.worker_type) {
    case HTML::WorkerType::Classic:
        // -> "classic"
        //    Fetch a classic worker script given job's serialized script url, job's client, "serviceworker", and the
        //    to-be-created environment settings object for this service worker.
        (void)HTML::fetch_classic_worker_script(job.script_url, job_client, Fetch::Infrastructure::Request::Destination::ServiceWorker, settings_object, perform_the_fetch_hook, on_fetch_complete);
        break;
    case HTML::WorkerType::Module:
        // -> "module"
        //    Fetch a module worker script graph given job's serialized script url, job's client, "serviceworker",
        //    "same-origin", and the to-be-created environment settings object for this service worker.
        (void)HTML::fetch_module_worker_script_graph(job.script_url, job_client, Fetch::Infrastructure::Request::Destination::ServiceWorker, settings_object, perform_the_fetch_hook, on_fetch_complete, Fetch::Infrastructure::Request::CredentialsMode::SameOrigin);
        break;
    }
}

// https://w3c.github.io/ServiceWorker/#dfn-bad-import-script-response
bool is_a_bad_import_script_response(Fetch::Infrastructure::ResponseView const& response)
{
    // A bad import script response is a response for which any of the following conditions are met:
    // - response's type is "error"
    // - response's status is not an ok status
    // - The result of extracting a MIME type from response's header list is not a JavaScript MIME type
    if (response.type() == Fetch::Infrastructure::Response::Type::Error || !Fetch::Infrastructure::is_ok_status(response.status()))
        return true;
    auto mime_type = Fetch::Infrastructure::extract_mime_type(response.header_list());
    return !mime_type.has_value() || !mime_type->is_javascript();
}

// https://w3c.github.io/ServiceWorker/#update-algorithm
void check_imported_scripts_for_update(HTML::EnvironmentSettingsObject& settings_object, Vector<ServiceWorkerScriptResource> const& newest_worker_scripts, UpdateImportedScriptsCheck check, OnImportedScriptsChecked on_complete)
{
    struct State : RefCounted<State> {
        void visit_edges(GC::Cell::Visitor& visitor) const
        {
            visitor.visit(on_complete);
        }

        size_t remaining_fetches { 0 };
        bool has_updated_resources { false };
        GC::Ptr<GC::Function<void(bool)>> on_complete;
    };
    auto state = adopt_ref(*new State);
    state->on_complete = on_complete;

    // 21. If hasUpdatedResources is false and newestWorker's classic scripts imported flag is set, then:
    // NOTE: The following checks to see if an imported script has been updated, since the main script has not changed.
    // 1. For each importUrl → storedResponse of newestWorker's script resource map:
    for (auto const& stored_script : newest_worker_scripts) {
        // 1. If importUrl is url, then continue.
        if (stored_script.url == check.script_url)
            continue;

        // 2. Let importRequest be a new request whose url is importUrl, client is job's client, destination is
        //    "script", parser metadata is "not parser-inserted", and whose use-URL-credentials flag is set.
        // NB: The client is the environment settings object of the worker the job creates.
        auto import_request = Fetch::Infrastructure::Request::create();
        import_request->set_url(stored_script.url);
        import_request->set_client(settings_object.address());
        import_request->set_destination(Fetch::Infrastructure::Request::Destination::Script);
        import_request->set_parser_metadata(Fetch::Infrastructure::Request::ParserMetadata::NotParserInserted);
        import_request->set_use_url_credentials(true);
        import_request->set_service_workers_mode(Fetch::Infrastructure::Request::ServiceWorkersMode::None);

        // 3. Set importRequest's cache mode to "no-cache" if any of the following are true:
        //    - registration's update via cache mode is "none".
        //    - job's force bypass cache flag is set.
        //    - registration is stale.
        if (check.update_via_cache == ServiceWorkerUpdateViaCache::None || check.force_bypass_cache || check.registration_is_stale)
            import_request->set_cache_mode(HTTP::CacheMode::NoCache);

        // 4. Let fetchedResponse be the result of fetching importRequest.
        ++state->remaining_fetches;
        Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input { Fetch::Engine::BodyIntent::ConsumeForCallback };
        auto stored_body = MUST(ByteBuffer::copy(stored_script.body));
        fetch_algorithms_input.process_response_consume_body = GC::create_function(GC::Heap::the(), GC::captures(state, move(stored_body)), [](auto& state, auto& stored_body, GC::Ref<Fetch::Infrastructure::ResponseView> response, Fetch::Infrastructure::FetchAlgorithms::BodyBytes body_bytes) {
            // 5. Set updatedResourceMap[importRequest's url] to fetchedResponse.
            // NB: The new worker fetches its imported scripts again as it runs.

            // 6. Set fetchedResponse to fetchedResponse's unsafe response.
            auto fetched_response = response->unsafe_response();

            // 7. If fetchedResponse's cache state is not "local", set registration's last update check time to the
            //    current time.
            // NB: The UI process sets the registration's last update check time once the job's scripts are fetched.

            // 8. If fetchedResponse is a bad import script response, continue.
            // NOTE: Bad responses for importScripts() are ignored for the purpose of the byte-to-byte check. Only good
            //       responses for the incumbent worker and good responses for the potential update worker are
            //       considered. See issue #1374 for some rationale.
            // 9. If fetchedResponse's body is not byte-for-byte identical with storedResponse's unsafe response's body,
            //    set hasUpdatedResources to true.
            // NOTE: The control does not break the loop in this step to continue with all the imported scripts to
            //       populate the cache.
            if (!is_a_bad_import_script_response(*fetched_response)) {
                auto const* body = body_bytes.get_pointer<Core::ImmutableBytes>();
                if (!body || body->bytes() != stored_body.bytes())
                    state->has_updated_resources = true;
            }

            if (--state->remaining_fetches == 0)
                state->on_complete->function()(state->has_updated_resources);
        });
        Fetch::Fetching::fetch(settings_object.realm(), import_request, Fetch::Infrastructure::FetchAlgorithms::create(move(fetch_algorithms_input)));
    }

    if (state->remaining_fetches == 0)
        state->on_complete->function()(false);
}

}
