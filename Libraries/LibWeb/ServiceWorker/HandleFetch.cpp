/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <LibGC/Root.h>
#include <LibWeb/Fetch/Fetching/ClientContextSnapshots.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/ServiceWorker/FetchChannel.h>
#include <LibWeb/ServiceWorker/HandleFetch.h>
#include <LibWebCommon/SecureContexts/AbstractOperations.h>

namespace Web::ServiceWorker {

namespace {

// A non-subresource request whose reserved client waits for the UI process to match a registration.
struct PendingMatch {
    GC::Root<GC::Function<void(Optional<ServiceWorkerControllerMatch>)>> on_match;
};

}

static HashMap<ServiceWorkerRequestId, PendingMatch>& pending_matches()
{
    // NB: The steps in here are cells of the agent whose fetch waits for a match.
    static thread_local NeverDestroyed<HashMap<ServiceWorkerRequestId, PendingMatch>> matches;
    return *matches;
}

// NB: A request ID only has to be unique among the requests of its agent, whose connection the answer comes back on.
static thread_local ServiceWorkerRequestId s_next_match_request_id { 0 };

// NB: Whether a service worker's set of event types to handle contains "fetch", as the UI process said when it matched
//     a registration whose active worker it is.
static HashMap<ServiceWorkerId, bool>& service_workers_handling_fetch_events()
{
    // NB: Each agent learns this from the matches of its own fetches.
    static thread_local NeverDestroyed<HashMap<ServiceWorkerId, bool>> service_workers;
    return *service_workers;
}

void forget_pending_matches_of_this_agent()
{
    pending_matches().clear();
    service_workers_handling_fetch_events().clear();
}

// https://w3c.github.io/ServiceWorker/#should-skip-event-algorithm
static bool should_skip_event(ServiceWorkerDescriptor const& service_worker)
{
    // 1. If serviceWorker's set of event types to handle does not contain eventName, then the user agent may return
    //    true.
    // NB: Only the set of a worker whose registration a reserved client matched is known here.
    if (service_workers_handling_fetch_events().get(service_worker.id) == false)
        return true;

    // 2. Return false.
    return false;
}

// https://w3c.github.io/ServiceWorker/#create-fetch-event-and-dispatch
static void create_fetch_event_and_dispatch(Page& page, NonnullRefPtr<Fetch::Infrastructure::Request> const& request, HTML::EnvironmentId const& client, ServiceWorkerDescriptor const& active_worker, HandleFetchCallback on_complete)
{
    // 1. Let response be null.
    // 2. Let eventCanceled be false.
    // 3. Let client be request's client.
    // 4. Let activeWorker be registration's active worker.
    // 5. Let eventHandled be null.
    // 6. Let handleFetchFailed be false.
    // 7. Let respondWithEntered be false.
    // 8. Let networkError be a network error.
    // FIXME: 9. If raceResponse is not null:
    // FIXME: 10. Let shouldSoftUpdate be true if any of the following are true, and false otherwise:

    // 11. If the result of running the Should Skip Event algorithm with "fetch" and activeWorker is true, then:
    if (should_skip_event(active_worker)) {
        // FIXME: 1. If shouldSoftUpdate is true, then in parallel run the Soft Update algorithm with registration.
        // 2. Return null.
        on_complete->function()(nullptr);
        return;
    }

    // FIXME: 12. If activeWorker's all fetch listeners are empty flag is set:
    // FIXME: 13. If useHighResPerformanceTimers is true, then set useHighResPerformanceTimers to activeWorker's global
    //            object's cross-origin isolated capability.
    // FIXME: 14. Let timingInfo's start time be the coarsened shared current time given useHighResPerformanceTimers.

    // 15. If activeWorker's state is "activating", wait for activeWorker's state to become "activated".
    // 16. If the result of running the Run Service Worker algorithm with activeWorker is failure, then set
    //     handleFetchFailed to true.
    // 17. Else:
    //     1. Set workerRealm to the relevant realm of the activeWorker's global object.
    //     2. Set eventHandled to a new promise in workerRealm.
    //     3. If raceResponse is not null, set activeWorker's global object's race response map[request] to raceResponse.
    //     4. Queue a task task to run the following substeps:
    // 18. Wait for task to have executed or for handleFetchFailed to be true.
    // NB: The UI process runs the worker, and the worker's process waits for it to be activated and runs the task. That
    //     process runs the steps that follow too, and reports what they return.
    // FIXME: 19. If shouldSoftUpdate is true, then in parallel run the Soft Update algorithm with registration.
    dispatch_fetch_event(page, client, active_worker, *request, on_complete);
}

// https://w3c.github.io/ServiceWorker/#on-fetch-request-algorithm
// NB: The steps after the registration and its active worker are known.
static void handle_fetch_with_registration(Page& page, NonnullRefPtr<Fetch::Infrastructure::Request> const& request, HTML::EnvironmentId const& client, ServiceWorkerDescriptor const& active_worker, HandleFetchCallback on_complete)
{
    // 11. Let activeWorker be registration's active worker.
    // FIXME: 12. Let shouldSoftUpdate be true if any of the following are true, and false otherwise:
    //            - request is a non-subresource request.
    //            - request is a subresource request and registration is stale.
    // FIXME: 13. If activeWorker's list of router rules is not empty:
    // 14. Let responseForAutoPreload be null.
    // FIXME: 15. If request is a navigation request, request's method is `GET`, registration's active worker's set of
    //            event types to handle contains fetch, and registration's active worker's all fetch listeners are
    //            empty flag is not set then:
    // 16. Else, resolve preloadResponse with undefined.
    // NB: The worker's process creates the fetch event's preloadResponse.

    // 17. Let fetchResult be the result of Create Fetch Event and Dispatch with request, registration,
    //     useHighResPerformanceTimers, timingInfo, workerRealm, reservedClient, preloadResponse, and
    //     responseForAutoPreload.
    // FIXME: 18. If timingInfo's worker final router source is not an empty string:
    // 19. Return fetchResult.
    create_fetch_event_and_dispatch(page, request, client, active_worker, on_complete);
}

// https://w3c.github.io/ServiceWorker/#on-fetch-request-algorithm
void handle_fetch(Page& page, NonnullRefPtr<Fetch::Infrastructure::Request> request, GC::Ref<Fetch::Infrastructure::FetchController>, bool, HandleFetchCallback on_complete)
{
    using Destination = Fetch::Infrastructure::Request::Destination;

    // AD-HOC: Service workers exist only where their interfaces are exposed, so no registration can match elsewhere.
    if (!HTML::WindowOrWorkerGlobalScopeMixin::expose_experimental_interfaces()) {
        on_complete->function()(nullptr);
        return;
    }

    // 1. Let registration be null.
    // 2. Let client be request's client.
    // 3. Let reservedClient be request's reserved client.
    auto reserved_client = request->reserved_client_snapshot();

    // FIXME: 4. Let preloadResponse be a new promise.
    // 5. Let workerRealm be null.
    // FIXME: 6. Let timingInfo be a new service worker timing info.

    // 7. Assert: request's destination is not "serviceworker".
    VERIFY(request->destination() != Destination::ServiceWorker);

    // 8. If request's destination is either "embed" or "object", then:
    if (request->destination() == Destination::Embed || request->destination() == Destination::Object) {
        // 1. Return null.
        on_complete->function()(nullptr);
        return;
    }

    // 9. Else if request is a non-subresource request, then:
    if (request->is_non_subresource_request()) {
        // 1. If reservedClient is not null and is an environment settings object, then:
        if (reserved_client && reserved_client->kind.has<Fetch::Infrastructure::ReservedClientContext::SettingsObject>()) {
            // 1. If reservedClient is not a secure context, return null.
            if (reserved_client->is_secure_context == Fetch::Infrastructure::ReservedClientContext::IsSecureContext::No) {
                on_complete->function()(nullptr);
                return;
            }
        }
        // 2. Else:
        else {
            // 1. If request's url is not a potentially trustworthy URL, return null.
            if (SecureContexts::is_url_potentially_trustworthy(request->url()) != SecureContexts::Trustworthiness::PotentiallyTrustworthy) {
                on_complete->function()(nullptr);
                return;
            }
        }

        // 3. If request is a navigation request and the navigation triggering it was initiated with a shift+reload or
        //    equivalent, return null.
        // NB: A navigation that bypasses the HTTP cache has the "reload" cache mode.
        if (request->is_navigation_request() && request->cache_mode() == HTTP::CacheMode::Reload) {
            on_complete->function()(nullptr);
            return;
        }

        // 4. Assert reservedClient is not null.
        // AD-HOC: A navigation of a navigable that this process does not host has no reserved client this process can
        //         name to the UI process, and so goes to the network.
        if (!reserved_client) {
            on_complete->function()(nullptr);
            return;
        }

        ServiceWorkerReservedClient reserved_client_to_match {
            .id = reserved_client->id,
            .creation_url = reserved_client->creation_url,
            .url = request->url(),
            .navigable_id = {},
        };
        Optional<HTML::EnvironmentSettingsObjectAddress> reserved_settings_object;
        reserved_client->kind.visit(
            [&](Fetch::Infrastructure::ReservedClientContext::Environment const& environment) {
                reserved_client_to_match.navigable_id = environment.navigable_id;
            },
            [&](Fetch::Infrastructure::ReservedClientContext::SettingsObject const& settings_object) {
                reserved_settings_object = settings_object.address;
            });
        if (!reserved_settings_object.has_value() && !reserved_client_to_match.navigable_id.has_value()) {
            on_complete->function()(nullptr);
            return;
        }

        // 5. Let storage key be the result of running obtain a storage key given reservedClient.
        // 6. Set registration to the result of running Match Service Worker Registration given storage key and
        //    request's url.
        // NB: The UI process holds the registrations, and obtains the storage key from its own record of the reserved
        //     client's creation URL and of the navigable or worker it is for.
        auto request_id = ++s_next_match_request_id;
        auto is_report = request->destination() == Destination::Report;
        auto on_match = GC::create_function(page.heap(), GC::captures(GC::Ref { page }, request, on_complete, reserved_client->id, reserved_settings_object, is_report), [](auto& page, auto& request, auto& on_complete, auto& reserved_client_id, auto& reserved_settings_object, auto& is_report, Optional<ServiceWorkerControllerMatch> match) {
            // 7. If registration is null or registration's active worker is null, return null.
            if (!match.has_value()) {
                on_complete->function()(nullptr);
                return;
            }

            // 8. If request's destination is not "report", set reservedClient's active service worker to registration's
            //    active worker.
            // NB: The UI process records the reserved client's active service worker, which a navigation's window takes
            //     from that record.
            if (!is_report && reserved_settings_object.has_value()) {
                if (auto settings_object = HTML::environment_settings_object_at(*reserved_settings_object))
                    settings_object->active_service_worker = match->active_worker;
            }
            service_workers_handling_fetch_events().set(match->active_worker.id, match->handles_fetch_events);

            // NOTE: From this point, the service worker client starts to use its active service worker's containing
            //       service worker registration.
            handle_fetch_with_registration(*page, request, reserved_client_id, match->active_worker, on_complete);
        });
        pending_matches().set(request_id, PendingMatch { GC::make_root(on_match) });
        if (!page.client().page_did_request_service_worker_match(request_id, reserved_client_to_match)) {
            pending_matches().remove(request_id);
            on_complete->function()(nullptr);
        }
        return;
    }

    // 10. Else if request is a subresource request, then:
    if (request->is_subresource_request()) {
        // 1. If client's active service worker is non-null, set registration to client's active service worker's
        //    containing service worker registration.
        // NB: A client that another process hosts is never controlled here.
        auto client = Fetch::Fetching::resolve_client(*request);
        if (client && client->active_service_worker.has_value()) {
            auto active_service_worker = *client->active_service_worker;
            handle_fetch_with_registration(page, request, client->id, active_service_worker, on_complete);
            return;
        }

        // 2. Else, return null.
        on_complete->function()(nullptr);
        return;
    }

    // AD-HOC: A request that is neither has no registration, and goes to the network.
    on_complete->function()(nullptr);
}

void service_worker_match_finished(ServiceWorkerRequestId request_id, Optional<ServiceWorkerControllerMatch> match)
{
    auto pending_match = pending_matches().take(request_id);
    if (!pending_match.has_value())
        return;
    pending_match->on_match->function()(move(match));
}

}
