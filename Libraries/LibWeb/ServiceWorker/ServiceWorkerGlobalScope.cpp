/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CookieStore/CookieStore.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/MIME.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/ServiceWorker/EventNames.h>
#include <LibWeb/ServiceWorker/ServiceWorkerGlobalScope.h>
#include <LibWebCommon/Fetch/Infrastructure/HTTP/Statuses.h>

namespace Web::ServiceWorker {

GC_DEFINE_ALLOCATOR(ServiceWorkerGlobalScope);

ServiceWorkerGlobalScope::~ServiceWorkerGlobalScope() = default;

ServiceWorkerGlobalScope::ServiceWorkerGlobalScope(GC::Ref<Web::Page> page)
    : HTML::WorkerGlobalScope(page)
{
}

void ServiceWorkerGlobalScope::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(m_cookie_store);
    for (auto const& [url, resource] : m_script_resource_map)
        visitor.visit(resource.response);
    visitor.visit(m_skip_waiting_promises);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-oninstall
void ServiceWorkerGlobalScope::set_oninstall(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(EventNames::install, value);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-oninstall
GC::Ptr<WebIDL::CallbackType> ServiceWorkerGlobalScope::oninstall()
{
    return event_handler_attribute(EventNames::install);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onactivate
void ServiceWorkerGlobalScope::set_onactivate(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(EventNames::activate, value);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onactivate
GC::Ptr<WebIDL::CallbackType> ServiceWorkerGlobalScope::onactivate()
{
    return event_handler_attribute(EventNames::activate);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onfetch
void ServiceWorkerGlobalScope::set_onfetch(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(EventNames::fetch, value);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onfetch
GC::Ptr<WebIDL::CallbackType> ServiceWorkerGlobalScope::onfetch()
{
    return event_handler_attribute(EventNames::fetch);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onmessage
void ServiceWorkerGlobalScope::set_onmessage(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(EventNames::message, value);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onmessage
GC::Ptr<WebIDL::CallbackType> ServiceWorkerGlobalScope::onmessage()
{
    return event_handler_attribute(EventNames::message);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onmessageerror
void ServiceWorkerGlobalScope::set_onmessageerror(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(EventNames::messageerror, value);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onmessageerror
GC::Ptr<WebIDL::CallbackType> ServiceWorkerGlobalScope::onmessageerror()
{
    return event_handler_attribute(EventNames::messageerror);
}

// https://cookiestore.spec.whatwg.org/#ServiceWorkerGlobalScope
GC::Ref<CookieStore::CookieStore> ServiceWorkerGlobalScope::cookie_store()
{
    // The cookieStore getter steps are to return this’s associated CookieStore.
    if (!m_cookie_store)
        m_cookie_store = GC::Heap::the().allocate<CookieStore::CookieStore>(page()->client());
    return *m_cookie_store;
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-skipwaiting
GC::Ref<WebIDL::Promise> ServiceWorkerGlobalScope::skip_waiting()
{
    // NOTE: The skipWaiting() method allows this service worker to progress from the registration's waiting position to
    //       active even while service worker clients are using the registration.

    // 1. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm());

    // 2. Run the following substeps in parallel:
    //    1. Set service worker's skip waiting flag.
    //    2. Invoke Try Activate with service worker's containing service worker registration.
    //    3. Queue a task on service worker's responsible event loop, to resolve promise with undefined.
    // NB: The UI process runs the substeps, and skip_waiting_finished() queues the task.
    auto request_id = ++m_next_skip_waiting_request_id;
    m_skip_waiting_promises.set(request_id, promise);
    page()->client().page_did_call_service_worker_skip_waiting(request_id);

    // 3. Return promise.
    return promise;
}

void ServiceWorkerGlobalScope::skip_waiting_finished(ServiceWorkerRequestId request_id)
{
    auto promise = m_skip_waiting_promises.take(request_id);
    if (!promise.has_value())
        return;
    HTML::queue_a_task(HTML::Task::Source::Unspecified, HTML::relevant_settings_object(*this).responsible_event_loop(), nullptr, GC::create_function(heap(), GC::captures(GC::Ref { *this }, *promise), [](auto& self, auto& promise) {
        HTML::TemporaryExecutionContext const context { self->realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        WebIDL::resolve_promise(self->realm(), promise, JS::js_undefined());
    }));
}

// https://w3c.github.io/ServiceWorker/#installation-algorithm
void ServiceWorkerGlobalScope::prune_script_resource_map()
{
    // 13. Let map be registration's installing worker's script resource map.
    // 14. Let usedSet be registration's installing worker's set of used scripts.
    // 15. For each url of map:
    //     1. If usedSet does not contain url, then remove map[url].
    m_script_resource_map.remove_all_matching([&](auto const& url, auto const&) {
        return !m_set_of_used_scripts.contains_slow(url);
    });
}

// https://w3c.github.io/ServiceWorker/#dfn-bad-import-script-response
static bool is_a_bad_import_script_response(Fetch::Infrastructure::ResponseView const& response)
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

// https://w3c.github.io/ServiceWorker/#importscripts
HTML::PerformTheFetchHook ServiceWorkerGlobalScope::import_scripts_perform_the_fetch_hook()
{
    // When the importScripts(urls) method is called on a ServiceWorkerGlobalScope object, the user agent must import
    // scripts into worker global scope, given this ServiceWorkerGlobalScope object and urls, and with the following
    // perform the fetch hook steps, given the request request:
    return GC::create_function(heap(), GC::captures(GC::Ref { *this }), [](auto& self, NonnullRefPtr<Fetch::Infrastructure::Request> request, HTML::TopLevelModule, HTML::ProcessCustomFetchResponse process_custom_fetch_response) -> WebIDL::ExceptionOr<void> {
        auto stored_response = [&](ScriptResource const& resource) {
            process_custom_fetch_response->function()(resource.response, resource.body);
        };

        // 1. Let serviceWorker be request's client's global object's service worker.
        // 2. Let map be serviceWorker's script resource map.
        // 3. Let url be request's url.
        auto url = request->url();

        // 4. If serviceWorker's state is not "parsed" or "installing":
        if (!first_is_one_of(self->m_service_worker_state, ServiceWorkerState::Parsed, ServiceWorkerState::Installing)) {
            // 1. Return map[url] if it exists and a network error otherwise.
            if (auto resource = self->m_script_resource_map.get(url); resource.has_value())
                stored_response(*resource);
            else
                process_custom_fetch_response->function()(Fetch::Infrastructure::ResponseView::network_error("The service worker has no stored script for this URL"_string), Empty {});
            return {};
        }

        // 5. If map[url] exists:
        if (auto resource = self->m_script_resource_map.get(url); resource.has_value()) {
            // 1. Append url to serviceWorker's set of used scripts.
            if (!self->m_set_of_used_scripts.contains_slow(url))
                self->m_set_of_used_scripts.append(url);

            // 2. Return map[url].
            stored_response(*resource);
            return {};
        }

        // 6. Let registration be serviceWorker's containing service worker registration.
        // 7. Set request's service-workers mode to "none".
        request->set_service_workers_mode(Fetch::Infrastructure::Request::ServiceWorkersMode::None);

        // 8. Set request's cache mode to "no-cache" if any of the following are true:
        //    - registration's update via cache mode is "none".
        //    - FIXME: The current global object's force bypass cache for import scripts flag is set.
        //    - FIXME: registration is stale.
        if (self->m_registration_update_via_cache == ServiceWorkerUpdateViaCache::None)
            request->set_cache_mode(HTTP::CacheMode::NoCache);

        // 9. Let response be the result of fetching request.
        Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input { Fetch::Engine::BodyIntent::ConsumeForCallback };
        fetch_algorithms_input.process_response_consume_body = GC::create_function(self->heap(), GC::captures(self, url, process_custom_fetch_response), [](auto& self, auto const& url, auto& process_custom_fetch_response_function, GC::Ref<Fetch::Infrastructure::ResponseView> response, Fetch::Infrastructure::FetchAlgorithms::BodyBytes body_bytes) {
            // FIXME: 10. If response's cache state is not "local", set registration's last update check time to the current
            //            time.

            // 11. If response's unsafe response is a bad import script response, then return a network error.
            if (is_a_bad_import_script_response(*response->unsafe_response()) || !body_bytes.has<Core::ImmutableBytes>()) {
                process_custom_fetch_response_function->function()(Fetch::Infrastructure::ResponseView::network_error("The imported script is a bad import script response"_string), Empty {});
                return;
            }

            // 12. Set map[url] to response.
            self->m_script_resource_map.set(url, ScriptResource { .response = response, .body = body_bytes.get<Core::ImmutableBytes>() });

            // 13. Append url to serviceWorker's set of used scripts.
            if (!self->m_set_of_used_scripts.contains_slow(url))
                self->m_set_of_used_scripts.append(url);

            // FIXME: 14. Set serviceWorker's classic scripts imported flag.

            // 15. Return response.
            process_custom_fetch_response_function->function()(response, move(body_bytes));
        });
        Fetch::Fetching::fetch(self->realm(), request, Fetch::Infrastructure::FetchAlgorithms::create(move(fetch_algorithms_input)));
        return {};
    });
}

}
