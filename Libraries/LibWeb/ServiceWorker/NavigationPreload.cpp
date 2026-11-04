/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibHTTP/HeaderList.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/ResponseView.h>
#include <LibWeb/Fetch/Response.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/ServiceWorker/NavigationPreload.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::ServiceWorker {

// https://w3c.github.io/ServiceWorker/#on-fetch-request-algorithm
GC::Ref<WebIDL::Promise> start_navigation_preload(HTML::EnvironmentSettingsObject& settings_object, Fetch::Infrastructure::Request const& request, ByteString const& header_value)
{
    auto& realm = settings_object.realm();
    HTML::TemporaryExecutionContext execution_context { realm };

    // 4. Let preloadResponse be a new promise.
    // NB: The service worker's realm is the one preloadResponse is the fetch event's attribute in.
    auto preload_response = WebIDL::create_promise(realm);

    // 15.1. If registration's navigation preload enabled flag is set then:
    // 1. Let preloadRequest be the result of cloning the request request.
    auto preload_request = request.clone();

    // NB: The worker's process makes the preload fetch, as the client of the request it makes.
    preload_request->set_client(settings_object.address());

    // 2. Let preloadRequestHeaders be preloadRequest's header list.
    auto preload_request_headers = preload_request->header_list();

    // 3. Let preloadResponseObject be a new Response object associated with a new Headers object whose guard is
    //    "immutable".
    // NB: The object is created once there is a response to associate it with.

    // 4. Append to preloadRequestHeaders a new header whose name is `Service-Worker-Navigation-Preload` and value is
    //    registration's navigation preload header value.
    preload_request_headers->append(HTTP::Header { .name = "Service-Worker-Navigation-Preload"sv, .value = header_value });

    // 5. Set preloadRequest's service-workers mode to "none".
    preload_request->set_service_workers_mode(Fetch::Infrastructure::Request::ServiceWorkersMode::None);

    // 6. Let preloadFetchController be null.
    // 7. Run the following substeps in parallel, but abort when fetchController's state is "terminated" or "aborted":
    //    1. Set preloadFetchController to the result of fetching preloadRequest.
    //       To processResponse for navigationPreloadResponse, run these substeps:
    // FIXME: Abort when fetchController's state is "terminated" or "aborted".
    Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input { Fetch::Engine::BodyIntent::DeliverStreamToConsumerAgent };
    fetch_algorithms_input.process_response = GC::create_function(realm.heap(), GC::captures(GC::Ref { realm }, preload_response), [](auto& realm, auto& preload_response, GC::Ref<Fetch::Infrastructure::ResponseView> navigation_preload_response) {
        HTML::TemporaryExecutionContext execution_context { *realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 1. If navigationPreloadResponse's type is "error", reject preloadResponse with a TypeError and terminate these
        //    substeps.
        if (navigation_preload_response->type() == Fetch::Infrastructure::Response::Type::Error) {
            WebIDL::reject_promise(*realm, preload_response, JS::TypeError::create(*realm, "The navigation preload failed"_utf16));
            return;
        }

        // 2. Associate preloadResponseObject with navigationPreloadResponse.
        auto preload_response_object = Fetch::Response::create(navigation_preload_response, Fetch::Headers::Guard::Immutable);

        // 3. Resolve preloadResponse with preloadResponseObject.
        WebIDL::resolve_promise(*realm, preload_response, Bindings::wrap(Bindings::host_defined_wrapper_world(*realm), *realm, preload_response_object));
    });
    (void)Fetch::Fetching::fetch(realm, preload_request, Fetch::Infrastructure::FetchAlgorithms::create(move(fetch_algorithms_input)));

    // FIXME: 8. If aborted, then:
    return preload_response;
}

}
