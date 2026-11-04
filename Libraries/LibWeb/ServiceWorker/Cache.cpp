/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibCore/ImmutableBytes.h>
#include <LibGC/Heap.h>
#include <LibGC/HeapVector.h>
#include <LibHTTP/HeaderList.h>
#include <LibJS/Runtime/Array.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/Fetch/Fetching/ClientContextSnapshots.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/ResponseView.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Fetch/Request.h>
#include <LibWeb/Fetch/Response.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/ServiceWorker/Cache.h>
#include <LibWeb/ServiceWorker/CacheStorage.h>
#include <LibWeb/ServiceWorker/FetchEventTypes.h>
#include <LibWeb/ServiceWorker/ServiceWorkerGlobalScope.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamDefaultReader.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::ServiceWorker {

GC_DEFINE_ALLOCATOR(Cache);

// NB: The CacheStorage object of the global that made an operation keeps the operation's steps alive. An operation that
//     the UI process never answers, such as one of a document it has discarded, is then collected with that global.
static HashMap<u64, GC::Weak<CacheStorage>>& pending_cache_storage_operations()
{
    // NB: The operations in here are those of the agent whose CacheStorage objects wait for their answers.
    static thread_local NeverDestroyed<HashMap<u64, GC::Weak<CacheStorage>>> operations;
    return *operations;
}

void forget_cache_storage_operations_of_this_agent()
{
    pending_cache_storage_operations().clear();
}

void request_cache_storage_operation(HTML::EnvironmentSettingsObject& settings, CacheStorageRequest request, GC::Ref<OnCacheStorageResult> on_result)
{
    // NB: A request ID only has to be unique among the requests of its agent, whose connection the answer comes back on.
    static thread_local u64 next_request_id = 0;
    auto request_id = ++next_request_id;

    auto* global_scope = HTML::window_or_worker_global_scope_from_global_object(settings.global_object());
    VERIFY(global_scope);
    auto cache_storage = global_scope->caches();
    cache_storage->add_pending_operation(request_id, on_result);

    auto& operations = pending_cache_storage_operations();
    operations.remove_all_matching([](u64, GC::Weak<CacheStorage> const& cache_storage) { return !cache_storage; });
    operations.set(request_id, cache_storage);

    Bindings::principal_host_defined_page(settings.realm()).client().page_did_request_cache_storage_operation(settings.id, request_id, request);
}

void cache_storage_operation_finished(u64 request_id, CacheStorageResult result)
{
    auto cache_storage = pending_cache_storage_operations().take(request_id);
    if (!cache_storage.has_value() || !*cache_storage)
        return;
    auto on_result = (*cache_storage)->take_pending_operation(request_id);
    if (!on_result)
        return;
    on_result->function()(move(result));
}

bool reject_with_exception_of(GC::Ref<WebIDL::Promise> promise, CacheStorageResult const& result)
{
    auto const* error = result.get_pointer<CacheStorageError>();
    if (!error)
        return false;
    auto& realm = WebIDL::promise_realm(promise);
    switch (*error) {
    case CacheStorageError::TypeError:
        WebIDL::reject_promise_with_exception(promise, WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "The cache operation failed"_utf16 });
        break;
    case CacheStorageError::InvalidStateError:
        WebIDL::reject_promise(promise, WebIDL::InvalidStateError::create(realm, "Batch operation requests must be unique"_utf16));
        break;
    case CacheStorageError::SecurityError:
        WebIDL::reject_promise(promise, WebIDL::SecurityError::create(realm, "The cache storage of this environment cannot be accessed"_utf16));
        break;
    }
    return true;
}

void queue_cache_task(GC::Ref<WebIDL::Promise> promise, GC::Ref<GC::Function<void()>> steps)
{
    HTML::queue_a_task(
        HTML::Task::Source::DOMManipulation,
        HTML::relevant_settings_object(promise->promise()).responsible_event_loop(),
        {},
        GC::create_function(GC::Heap::the(), GC::captures(promise, steps), [](auto& promise, auto& steps) {
            HTML::TemporaryExecutionContext context { WebIDL::promise_realm(promise), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
            steps->function()();
        }));
}

CachedRequest describe_cached_request(Fetch::Infrastructure::Request const& request)
{
    Optional<u8> destination;
    if (request.destination().has_value())
        destination = static_cast<u8>(to_underlying(*request.destination()));
    return CachedRequest {
        .method = request.method(),
        .url_list = request.url_list(),
        .header_list = request.header_list()->headers(),
        .destination = destination,
        .mode = static_cast<u8>(to_underlying(request.mode())),
        .credentials_mode = static_cast<u8>(to_underlying(request.credentials_mode())),
        .cache_mode = request.cache_mode(),
        .redirect_mode = static_cast<u8>(to_underlying(request.redirect_mode())),
        .referrer = request.referrer(),
        .referrer_policy = request.referrer_policy(),
        .integrity_metadata = request.integrity_metadata().to_utf8_but_should_be_ported_to_utf16(),
        .keepalive = request.keepalive(),
        .reload_navigation = request.reload_navigation(),
        .history_navigation = request.history_navigation(),
    };
}

CachedResponse describe_cached_response(Fetch::Infrastructure::ResponseView const& response)
{
    auto description = describe_response(response);
    return CachedResponse {
        .type = static_cast<u8>(to_underlying(description.type)),
        .filtered_header_list = response.header_list()->headers(),
        .status = description.status,
        .status_message = move(description.status_message),
        .header_list = move(description.header_list),
        .url_list = move(description.url_list),
        .cors_exposed_header_name_list = move(description.cors_exposed_header_name_list),
        .range_requested = description.range_requested,
        .request_includes_credentials = description.request_includes_credentials,
        .timing_allow_passed = description.timing_allow_passed,
        .redirect_taint = description.redirect_taint,
        .body = {},
    };
}

CacheQueryParameters cache_query_parameters(CacheQueryOptions const& options)
{
    return CacheQueryParameters {
        .ignore_search = options.ignore_search,
        .ignore_method = options.ignore_method,
        .ignore_vary = options.ignore_vary,
    };
}

template<typename Enum>
static Optional<Enum> enum_from(u8 value, Enum last)
{
    if (static_cast<int>(value) > static_cast<int>(to_underlying(last)))
        return {};
    return static_cast<Enum>(value);
}

RefPtr<Fetch::Infrastructure::Request> request_from_cached_request(CachedRequest const& description)
{
    using Request = Fetch::Infrastructure::Request;
    auto mode = enum_from(description.mode, Request::Mode::WebSocket);
    auto credentials_mode = enum_from(description.credentials_mode, Request::CredentialsMode::Include);
    auto redirect_mode = enum_from(description.redirect_mode, Request::RedirectMode::Manual);
    Optional<Request::Destination> destination;
    if (description.destination.has_value()) {
        destination = enum_from(*description.destination, Request::Destination::XSLT);
        if (!destination.has_value())
            return nullptr;
    }
    if (description.url_list.is_empty() || !mode.has_value() || !credentials_mode.has_value() || !redirect_mode.has_value())
        return nullptr;

    auto request = Request::create();
    request->set_method(description.method);
    request->set_url_list(description.url_list);
    request->set_header_list(HTTP::HeaderList::create(description.header_list));
    request->set_destination(destination);
    request->set_mode(*mode);
    request->set_credentials_mode(*credentials_mode);
    request->set_cache_mode(description.cache_mode);
    request->set_redirect_mode(*redirect_mode);
    request->set_referrer(description.referrer);
    request->set_referrer_policy(description.referrer_policy);
    request->set_integrity_metadata(Utf16String::from_utf8(description.integrity_metadata));
    request->set_keepalive(description.keepalive);
    request->set_reload_navigation(description.reload_navigation);
    request->set_history_navigation(description.history_navigation);
    return request;
}

GC::Ref<Fetch::Response> response_object_from(JS::Realm& realm, CachedResponse const& description)
{
    using Response = Fetch::Infrastructure::Response;
    NonnullRefPtr<Response> response = Response::network_error("The cached response is a network error"_string);
    if (auto type = enum_from(description.type, Response::Type::OpaqueRedirect); type.has_value() && *type != Response::Type::Error) {
        auto internal_response = internal_response_from(FetchEventResponse {
            .type = *type,
            .status = description.status,
            .status_message = description.status_message,
            .header_list = description.header_list,
            .url_list = description.url_list,
            .cors_exposed_header_name_list = description.cors_exposed_header_name_list,
            .range_requested = description.range_requested,
            .request_includes_credentials = description.request_includes_credentials,
            .timing_allow_passed = description.timing_allow_passed,
            .redirect_taint = description.redirect_taint,
            .has_body = description.body.has_value(),
        });
        if (description.body.has_value())
            internal_response->set_body(Fetch::Infrastructure::ResponseBody::from_bytes(MUST(Core::ImmutableBytes::copy(*description.body))));
        response = response_of_type(*type, move(internal_response));
    }
    return Fetch::Response::create(Fetch::Infrastructure::ResponseView::create(realm, response), Fetch::Headers::Guard::Immutable);
}

Cache::Cache(CacheId request_response_list)
    : m_request_response_list(request_response_list)
{
}

// https://w3c.github.io/ServiceWorker/#cache-match
void Cache::match(JS::Realm& realm, Fetch::RequestInfo request, CacheQueryOptions options, GC::Ref<WebIDL::Promise> promise)
{
    // 2. Run these substeps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), GC::captures(*this, realm, promise, move(request), options), [](auto& self, auto& realm, auto& promise, auto& request, auto& options) {
        HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 1. Let p be the result of running the algorithm specified in matchAll(request, options) method with request and options.
        // 2. Wait until p settles.
        auto match_all_promise = WebIDL::create_promise(realm);
        self.match_all(realm, move(request), options, match_all_promise);
        WebIDL::react_to_promise(match_all_promise,
            // 4. Else if p resolves with an array, responses, then:
            GC::create_function(GC::Heap::the(), GC::captures(realm, promise), [](auto& realm, auto& promise, JS::Value value) -> WebIDL::ExceptionOr<JS::Value> {
                HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                // 1. If responses is an empty array, then:
                if (auto& responses = value.as<JS::Array>(); responses.indexed_array_like_size() == 0) {
                    // 1. Resolve promise with undefined.
                    WebIDL::resolve_promise(promise);
                }
                // 2. Else:
                else {
                    // 1. Resolve promise with the first element of responses.
                    auto first_element = TRY(responses.get(0));
                    WebIDL::resolve_promise(promise, first_element);
                }

                return JS::js_undefined();
            }),

            // 3. If p rejects with an exception, then:
            GC::create_function(GC::Heap::the(), GC::captures(realm, promise), [](auto& realm, auto& promise, JS::Value exception) -> WebIDL::ExceptionOr<JS::Value> {
                HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                // 1. Reject promise with that exception.
                WebIDL::reject_promise(promise, exception);

                return JS::js_undefined();
            }));
    }));
}

// NB: The steps of matchAll() and keys() that find r: whether they returned a promise already, and r if they did not.
static Optional<RefPtr<Fetch::Infrastructure::Request>> request_of_query(JS::Realm& realm, Optional<Fetch::RequestInfo> const& request, CacheQueryOptions const& options, GC::Ref<WebIDL::Promise> promise, JS::Value value_for_non_get_request)
{
    // 1. Let r be null.
    RefPtr<Fetch::Infrastructure::Request> inner_request;

    // 2. If the optional argument request is not omitted, then:
    if (!request.has_value())
        return inner_request;

    bool completed_synchronously = false;
    request->visit(
        // 1. If request is a Request object, then:
        [&](GC::Ref<Fetch::Request> const& request) {
            // 1. Set r to request's request.
            inner_request = request->request();

            // 2. If r's method is not `GET` and options.ignoreMethod is false, return a promise resolved with an empty
            //    array.
            if (inner_request->method() != "GET"sv && !options.ignore_method) {
                WebIDL::resolve_promise(promise, value_for_non_get_request);
                completed_synchronously = true;
            }
        },
        // 2. Else if request is a string, then:
        [&](Utf16String const& request) {
            // 1. Set r to the associated request of the result of invoking the initial value of Request as constructor
            //    with request as its argument. If this throws an exception, return a promise rejected with that
            //    exception.
            auto request_object = Fetch::Request::create_with_settings(HTML::principal_realm_settings_object(realm), request);
            if (request_object.is_error()) {
                WebIDL::reject_promise_with_exception(promise, request_object.release_error());
                completed_synchronously = true;
                return;
            }
            inner_request = request_object.value()->request();
        });

    if (completed_synchronously)
        return {};
    return inner_request;
}

// https://w3c.github.io/ServiceWorker/#cache-matchall
void Cache::match_all(JS::Realm& realm, Optional<Fetch::RequestInfo> request, CacheQueryOptions options, GC::Ref<WebIDL::Promise> promise)
{
    // 1. Let r be null.
    // 2. If the optional argument request is not omitted, then:
    auto inner_request = request_of_query(realm, request, options, promise, MUST(JS::Array::create(realm, 0)));
    if (!inner_request.has_value())
        return;

    // 3. Let realm be this's relevant realm.
    // 4. Let promise be a new promise.
    // 5. Run these substeps in parallel:
    //    1. Let responses be an empty list.
    //    2. If the optional argument request is omitted, then:
    //       1. For each requestResponse of the relevant request response list:
    //          1. Add a copy of requestResponse's response to responses.
    //    3. Else:
    //       1. Let requestResponses be the result of running Query Cache with r and options.
    //       2. For each requestResponse of requestResponses:
    //          1. Add a copy of requestResponse's response to responses.
    // NB: The UI process holds the relevant request response list, and finds the responses.
    Optional<CachedRequest> request_query;
    if (*inner_request)
        request_query = describe_cached_request(**inner_request);
    CacheQueryRequest query {
        .cache = m_request_response_list,
        .kind = CacheQueryKind::MatchAll,
        .request = move(request_query),
        .options = cache_query_parameters(options),
    };
    request_cache_storage_operation(HTML::principal_realm_settings_object(realm), { move(query) }, GC::create_function(GC::Heap::the(), GC::captures(promise), [](auto& promise, CacheStorageResult result) {
        queue_cache_task(promise, GC::create_function(GC::Heap::the(), GC::captures(promise, move(result)), [](auto& promise, auto& result) {
            if (reject_with_exception_of(promise, result))
                return;
            auto& realm = WebIDL::promise_realm(promise);

            // 3. For each response of responses:
            //    1. If response's type is "opaque" and cross-origin resource policy check with promise's relevant
            //       settings object's origin, promise's relevant settings object, "", and response's internal response
            //       returns blocked, then reject promise with a TypeError and abort these steps.
            // FIXME: Perform the cross-origin resource policy check.

            // 4. Queue a task, on promise's relevant settings object's responsible event loop using the DOM
            //    manipulation task source, to perform the following steps:
            //    1. Let responseList be a list.
            //    2. For each response in responses:
            //       1. Add a new Response object associated with response and a new Headers object whose guard is
            //          "immutable" to responseList.
            //    3. Resolve promise with a frozen array created from responseList, in realm.
            auto response_list = GC::Heap::the().allocate<GC::HeapVector<JS::Value>>();
            for (auto const& item : result.template get<Vector<CachedRequestResponse>>())
                response_list->elements().append(Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, response_object_from(realm, item.response)));
            WebIDL::resolve_promise(promise, JS::Array::create_from(realm, response_list->elements()));
        }));
    }));

    // 6. Return promise.
}

// https://w3c.github.io/ServiceWorker/#cache-add
void Cache::add(JS::Realm& realm, Fetch::RequestInfo request, GC::Ref<WebIDL::Promise> promise)
{
    // 1. Let requests be an array containing only request.
    // 2. Let responseArrayPromise be the result of running the algorithm specified in addAll(requests) passing requests
    //    as the argument.
    auto add_all_promise = WebIDL::create_promise(realm);
    add_all(realm, { { request } }, add_all_promise);

    // 3. Return the result of reacting to responseArrayPromise with a fulfillment handler that returns undefined.
    WebIDL::react_to_promise(add_all_promise,
        GC::create_function(GC::Heap::the(), GC::captures(promise), [](auto& promise, JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
            WebIDL::resolve_promise(promise);
            return JS::js_undefined();
        }),
        GC::create_function(GC::Heap::the(), GC::captures(promise), [](auto& promise, JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
            WebIDL::reject_promise(promise, reason);
            return JS::js_undefined();
        }));
}

// NB: Runs the substeps of a method that invoke Batch Cache Operations in parallel, in the UI process, and settles the
//     promise with the outcome: undefined for put(), addAll() and add(), and whether an item was removed for delete().
void Cache::run_batch_cache_operations(Vector<CacheBatchOperationDescriptor> operations, GC::Ref<WebIDL::Promise> promise)
{
    bool is_delete = !operations.is_empty() && operations.first().type == CacheBatchOperationType::Delete;

    // 1. Let errorData be null.
    // 2. Invoke Batch Cache Operations with operations. If this throws an exception, set errorData to the exception.
    CacheBatchRequest batch { .cache = m_request_response_list, .operations = move(operations) };
    request_cache_storage_operation(HTML::relevant_settings_object(promise->promise()), { move(batch) }, GC::create_function(GC::Heap::the(), GC::captures(promise, is_delete), [](auto& promise, auto& is_delete, CacheStorageResult result) {
        // 3. Queue a task, on cacheJobPromise's relevant settings object's responsible event loop using the DOM
        //    manipulation task source, to perform the following substeps:
        queue_cache_task(promise, GC::create_function(GC::Heap::the(), GC::captures(promise, is_delete, move(result)), [](auto& promise, auto& is_delete, auto& result) {
            // 2. Else, reject cacheJobPromise with a new exception with errorData, in realm.
            if (reject_with_exception_of(promise, result))
                return;

            // 1. If errorData is null, resolve cacheJobPromise with undefined.
            // NB: delete() resolves cacheJobPromise with whether requestResponses is not empty.
            if (is_delete)
                WebIDL::resolve_promise(promise, JS::Value { result.template get<bool>() });
            else
                WebIDL::resolve_promise(promise);
        }));
    }));
}

// https://w3c.github.io/ServiceWorker/#cache-addAll
void Cache::add_all(JS::Realm& realm, ReadonlySpan<Fetch::RequestInfo> requests, GC::Ref<WebIDL::Promise> promise)
{
    // 1. Let responsePromises be an empty list.
    auto response_promises = GC::Heap::the().allocate<GC::HeapVector<GC::Ref<WebIDL::Promise>>>();

    // 2. Let requestList be an empty list.
    Vector<NonnullRefPtr<Fetch::Infrastructure::Request>> request_list;

    // 3. For each request whose type is Request in requests:
    for (auto const& request_info : requests) {
        if (auto const* request = request_info.get_pointer<GC::Ref<Fetch::Request>>()) {
            // 1. Let r be request’s request.
            auto inner_request = (*request)->request();

            // 2. If r’s url’s scheme is not one of "http" and "https", or r’s method is not `GET`, return a promise
            //    rejected with a TypeError.
            if (!inner_request->url().scheme().is_one_of("http"sv, "https"sv) || inner_request->method() != "GET"sv) {
                WebIDL::reject_promise(promise, JS::TypeError::create(realm, "Request must be a GET request with an HTTP(S) URL"sv));
                return;
            }
        }
    }

    // 4. Let fetchControllers be a list of fetch controllers.
    auto fetch_controllers = GC::Heap::the().allocate<GC::HeapVector<GC::Ref<Fetch::Infrastructure::FetchController>>>();

    // 5. For each request in requests:
    for (auto const& request_info : requests) {
        // 1. Let r be the associated request of the result of invoking the initial value of Request as constructor with
        //    request as its argument. If this throws an exception, return a promise rejected with that exception.
        auto result = Fetch::Request::create_with_settings(HTML::principal_realm_settings_object(realm), request_info);
        if (result.is_error()) {
            WebIDL::reject_promise_with_exception(promise, result.release_error());
            return;
        }

        auto inner_request = result.value()->request();

        // 2. If r’s url’s scheme is not one of "http" and "https", then:
        if (!inner_request->url().scheme().is_one_of("http"sv, "https"sv)) {
            // 1. For each fetchController of fetchControllers, abort fetchController.
            for (auto fetch_controller : fetch_controllers->elements())
                fetch_controller->abort(realm, {});

            // 2. Return a promise rejected with a TypeError.
            WebIDL::reject_promise(promise, JS::TypeError::create(realm, "Request must have an HTTP(S) URL"sv));
            return;
        }

        // 3. If r’s client’s global object is a ServiceWorkerGlobalScope object, set request’s service-workers mode to "none".
        auto client = Fetch::Fetching::resolve_client(inner_request);
        if (Bindings::service_worker_global_scope_from_global_object(client->global_object()))
            inner_request->set_service_workers_mode(Fetch::Infrastructure::Request::ServiceWorkersMode::None);

        // 4. Add r to requestList.
        request_list.append(inner_request);

        // 5. Let responsePromise be a new promise.
        auto response_promise = WebIDL::create_promise(realm);

        // 6. Run the following substeps in parallel:
        Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), GC::captures(realm, fetch_controllers, inner_request, response_promise), [](auto& realm, auto& fetch_controllers, auto& inner_request, auto& response_promise) {
            // * Append the result of fetching r.
            Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input { Fetch::Engine::BodyIntent::DeliverStreamToConsumerAgent };

            // * To processResponse for response, run these substeps:
            fetch_algorithms_input.process_response = GC::create_function(GC::Heap::the(), GC::captures(realm, fetch_controllers, response_promise), [](auto& realm, auto& fetch_controllers, auto& response_promise, GC::Ref<Fetch::Infrastructure::ResponseView> response) {
                HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
                bool did_reject_promise = false;

                // 1. If response’s type is "error", or response’s status is not an ok status or is 206, reject
                //    responsePromise with a TypeError.
                auto status = response->status();
                if (response->type() == Fetch::Infrastructure::Response::Type::Error || !Fetch::Infrastructure::is_ok_status(status) || status == 206) {
                    WebIDL::reject_promise(response_promise, JS::TypeError::create(realm, "Fetch request failed"_utf16));
                    did_reject_promise = true;
                }
                // 2. Else if response’s header list contains a header named `Vary`, then:
                else if (response->header_list()->contains("Vary"sv)) {
                    // 1. Let fieldValues be the list containing the elements corresponding to the field-values of the
                    //    Vary header.
                    // 2. For each fieldValue of fieldValues:
                    response->header_list()->for_each_vary_header([&](StringView field_value) {
                        // 1. If fieldValue matches "*", then:
                        if (field_value == "*"sv) {
                            // 1. Reject responsePromise with a TypeError.
                            WebIDL::reject_promise(response_promise, JS::TypeError::create(realm, "Vary '*' is not supported"_utf16));
                            did_reject_promise = true;

                            // 2. For each fetchController of fetchControllers, abort fetchController.
                            for (auto fetch_controller : fetch_controllers->elements())
                                fetch_controller->abort(realm, {});

                            // 3. Abort these steps.
                            return IterationDecision::Break;
                        }

                        return IterationDecision::Continue;
                    });
                }

                if (did_reject_promise)
                    return;

                // * To processResponseEndOfBody for response, run these substeps:
                // FIXME: Spec issue? processResponseEndOfBody is not invoked until the response's body is read, but
                //        doing so here locks the body's stream, thus cloning the response during batch operations fails.
                //        So we resolve the pending promise here, which seems to work fine.

                // 1. If response’s aborted flag is set, reject responsePromise with an "AbortError" DOMException and
                //    abort these steps.
                if (response->aborted()) {
                    WebIDL::reject_promise(response_promise, WebIDL::AbortError::create("Fetch request was aborted"_utf16));
                    return;
                }

                // 2. Resolve responsePromise with response.
                WebIDL::resolve_promise(response_promise, response);
            });

            auto fetch_controller = Fetch::Fetching::fetch(realm, inner_request, Fetch::Infrastructure::FetchAlgorithms::create(move(fetch_algorithms_input)));
            fetch_controllers->elements().append(fetch_controller);

            // Note: The cache commit is allowed when the response’s body is fully received.
        }));

        // 7. Add responsePromise to responsePromises.
        response_promises->elements().append(response_promise);
    }

    // 6. Let p be the result of getting a promise to wait for all of responsePromises.
    auto wait_for_responses_promise = WebIDL::get_promise_for_wait_for_all(realm, response_promises->elements());

    // 7. Return the result of reacting to p with a fulfillment handler that, when called with argument responses, performs the following substeps:
    WebIDL::react_to_promise(wait_for_responses_promise,
        GC::create_function(GC::Heap::the(), GC::captures(*this, realm, move(request_list), promise), [](auto& self, auto& realm, auto& request_list, auto& promise, JS::Value result) -> WebIDL::ExceptionOr<JS::Value> {
            HTML::TemporaryExecutionContext context { realm };
            auto& responses = result.as<JS::Array>();

            // 1. Let operations be an empty list.
            struct Operations : RefCounted<Operations> {
                GC_HOLDS_NO_CELL_EDGES;

                Vector<CacheBatchOperationDescriptor> operations;
                size_t bodies_to_read { 0 };
                bool failed { false };
            };
            auto operations = adopt_ref(*new Operations);

            // 2. Let index be zero.
            // 3. For each response in responses:
            auto response_views = GC::Heap::the().allocate<GC::HeapVector<GC::Ref<Fetch::Infrastructure::ResponseView>>>();
            for (size_t index = 0; index < responses.indexed_array_like_size(); ++index) {
                auto response_value = TRY(responses.get(index));
                auto& response = as<Fetch::Infrastructure::ResponseView>(response_value.as_cell());
                response_views->elements().append(response);

                // 1. Let operation be a cache batch operation.
                // 2. Set operation's type to "put".
                // 3. Set operation's request to requestList[index].
                // 4. Set operation's response to response.
                // 5. Append operation to operations.
                // 6. Increment index by one.
                operations->operations.append(CacheBatchOperationDescriptor {
                    .type = CacheBatchOperationType::Put,
                    .request = describe_cached_request(request_list[index]),
                    .response = describe_cached_response(response),
                    .options = {},
                });
            }

            // 5. Let cacheJobPromise be a new promise.
            // 6. Run the following substeps in parallel:
            // NB: The UI process stores the bytes of the responses' bodies, which are read first.
            auto run_operations = GC::create_function(GC::Heap::the(), GC::captures(self, operations, promise), [](auto& self, auto& operations, auto& promise) {
                self.run_batch_cache_operations(move(operations->operations), promise);
            });
            for (size_t index = 0; index < response_views->elements().size(); ++index) {
                if (response_views->elements()[index]->body())
                    ++operations->bodies_to_read;
            }
            if (operations->bodies_to_read == 0) {
                run_operations->function()();
                return JS::js_undefined();
            }
            for (size_t index = 0; index < response_views->elements().size(); ++index) {
                auto body = response_views->elements()[index]->body();
                if (!body)
                    continue;
                body->fully_read(realm,
                    GC::create_function(GC::Heap::the(), GC::captures(operations, index, run_operations), [](auto& operations, auto& index, auto& run_operations, ByteBuffer bytes) {
                        if (operations->failed)
                            return;
                        operations->operations[index].response->body = move(bytes);
                        if (--operations->bodies_to_read == 0)
                            run_operations->function()();
                    }),
                    GC::create_function(GC::Heap::the(), GC::captures(operations, promise), [](auto& operations, auto& promise, JS::Value error) {
                        if (exchange(operations->failed, true))
                            return;
                        WebIDL::reject_promise(promise, error);
                    }),
                    GC::Ref { realm.global_object() });
            }

            return JS::js_undefined();
        }),
        GC::create_function(GC::Heap::the(), GC::captures(promise), [](auto& promise, JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
            WebIDL::reject_promise(promise, reason);
            return JS::js_undefined();
        }));
}

// https://w3c.github.io/ServiceWorker/#cache-put
void Cache::put(JS::Realm& realm, Fetch::RequestInfo request, GC::Ref<Fetch::Response> response, GC::Ref<WebIDL::Promise> promise)
{
    // 1. Let innerRequest be null.
    RefPtr<Fetch::Infrastructure::Request> inner_request;

    auto request_result = request.visit(
        // 2. If request is a Request object, then set innerRequest to request’s request.
        [&](GC::Ref<Fetch::Request> request) -> WebIDL::ExceptionOr<void> {
            inner_request = request->request();
            return {};
        },
        // 3. Else:
        [&](Utf16String const& request) -> WebIDL::ExceptionOr<void> {
            // 1. Let requestObj be the result of invoking Request’s constructor with request as its argument. If this
            //    throws an exception, return a promise rejected with exception.
            auto request_object = Fetch::Request::create_with_settings(HTML::principal_realm_settings_object(realm), request);
            if (request_object.is_error()) {
                WebIDL::reject_promise_with_exception(promise, request_object.release_error());
                return {};
            }

            inner_request = request_object.value()->request();
            return {};
        });

    if (request_result.is_error()) {
        WebIDL::reject_promise_with_exception(promise, request_result.release_error());
        return;
    }

    if (!inner_request)
        return;

    // 4. If innerRequest’s url’s scheme is not one of "http" and "https", or innerRequest’s method is not `GET`, return
    //    a promise rejected with a TypeError.
    if (!inner_request->url().scheme().is_one_of("http"sv, "https"sv) || inner_request->method() != "GET"sv) {
        WebIDL::reject_promise(promise, JS::TypeError::create(realm, "Request must be a GET request with an HTTP(S) URL"sv));
        return;
    }

    // 5. Let innerResponse be response’s response.
    auto inner_response = response->response();

    // 6. If innerResponse’s status is 206, return a promise rejected with a TypeError.
    if (inner_response->status() == 206) {
        WebIDL::reject_promise(promise, JS::TypeError::create(realm, "Partial responses are not supported"sv));
        return;
    }

    // 7. If innerResponse’s header list contains a header named `Vary`, then:
    //     1. Let fieldValues be the list containing the items corresponding to the Vary header’s field-values.
    //     2. For each fieldValue in fieldValues:
    bool found_vary_wildcard = false;

    inner_response->header_list()->for_each_vary_header([&](StringView field_value) {
        // 1. If fieldValue matches "*", return a promise rejected with a TypeError.
        found_vary_wildcard = field_value == "*"sv;
        return found_vary_wildcard ? IterationDecision::Break : IterationDecision::Continue;
    });

    if (found_vary_wildcard) {
        WebIDL::reject_promise(promise, JS::TypeError::create(realm, "Vary '*' is not supported"sv));
        return;
    }

    // 8. If innerResponse’s body is disturbed or locked, return a promise rejected with a TypeError.
    if (auto body = inner_response->body()) {
        if (auto stream = body->stream(); stream->is_disturbed() || stream->is_locked()) {
            WebIDL::reject_promise(promise, JS::TypeError::create(realm, "Response's body stream is disturbed or locked"sv));
            return;
        }
    }

    // 9. Let clonedResponse be a clone of innerResponse.
    // NB: The UI process stores what clonedResponse would be, with the bytes of innerResponse's body.
    // 12. Let operations be an empty list.
    // 13. Let operation be a cache batch operation.
    // 14. Set operation's type to "put".
    // 15. Set operation's request to innerRequest.
    // 16. Set operation's response to clonedResponse.
    // 17. Append operation to operations.
    Vector<CacheBatchOperationDescriptor> operations;
    operations.append(CacheBatchOperationDescriptor {
        .type = CacheBatchOperationType::Put,
        .request = describe_cached_request(*inner_request),
        .response = describe_cached_response(inner_response),
        .options = {},
    });

    // 19. Return the result of the fulfillment of bodyReadPromise:
    //     1. Let cacheJobPromise be a new promise.
    //     2. Return cacheJobPromise and run these steps in parallel:
    auto body = inner_response->body();

    // NB: An opaque filtered response has no body, but clonedResponse keeps the body of its internal response. The
    //     bytes of that body are read from a clone, which leaves innerResponse's internal response its body.
    if (!body && inner_response->type() == Fetch::Infrastructure::Response::Type::Opaque)
        body = inner_response->clone(realm)->unsafe_response()->body();

    if (!body) {
        run_batch_cache_operations(move(operations), promise);
        return;
    }

    // 10. Let bodyReadPromise be a promise resolved with undefined.
    // 11. If innerResponse's body is non-null, run these substeps:
    //     1. Let stream be innerResponse's body's stream.
    //     2. Let reader be the result of getting a reader for stream.
    //     3. Set bodyReadPromise to the result of reading all bytes from reader.
    // NB: This ensures that innerResponse's body is locked, and we have a full buffered copy of the body in
    //     clonedResponse. An implementation could optimize by streaming directly to disk rather than memory.
    auto reader = MUST(body->stream()->get_a_reader());
    reader->read_all_bytes(
        GC::create_function(GC::Heap::the(), GC::captures(*this, move(operations), promise), [](auto& self, auto& operations, auto& promise, ByteBuffer bytes) {
            operations.first().response->body = move(bytes);
            self.run_batch_cache_operations(move(operations), promise);
        }),
        GC::create_function(GC::Heap::the(), GC::captures(promise), [](auto& promise, JS::Value reason) {
            WebIDL::reject_promise(promise, reason);
        }));
}

// https://w3c.github.io/ServiceWorker/#cache-delete
void Cache::delete_(JS::Realm& realm, Fetch::RequestInfo request, CacheQueryOptions options, GC::Ref<WebIDL::Promise> promise)
{
    // 1. Let r be null.
    // 2. If request is a Request object, then:
    //    1. Set r to request's request.
    //    2. If r's method is not `GET` and options.ignoreMethod is false, return a promise resolved with false.
    // 3. Else if request is a string, then:
    //    1. Set r to the associated request of the result of invoking the initial value of Request as constructor with
    //       request as its argument. If this throws an exception, return a promise rejected with that exception.
    auto inner_request = request_of_query(realm, move(request), options, promise, JS::Value { false });
    if (!inner_request.has_value())
        return;

    // 4. Let operations be an empty list.
    // 5. Let operation be a cache batch operation.
    // 6. Set operation's type to "delete".
    // 7. Set operation's request to r.
    // 8. Set operation's options to options.
    // 9. Append operation to operations.
    Vector<CacheBatchOperationDescriptor> operations;
    operations.append(CacheBatchOperationDescriptor {
        .type = CacheBatchOperationType::Delete,
        .request = describe_cached_request(**inner_request),
        .response = {},
        .options = cache_query_parameters(options),
    });

    // 10. Let realm be this's relevant realm.
    // 11. Let cacheJobPromise be a new promise.
    // 12. Run the following substeps in parallel:
    run_batch_cache_operations(move(operations), promise);

    // 13. Return cacheJobPromise.
}

// https://w3c.github.io/ServiceWorker/#cache-keys
void Cache::keys(JS::Realm& realm, Optional<Fetch::RequestInfo> request, CacheQueryOptions options, GC::Ref<WebIDL::Promise> promise)
{
    // 1. Let r be null.
    // 2. If the optional argument request is not omitted, then:
    auto inner_request = request_of_query(realm, request, options, promise, MUST(JS::Array::create(realm, 0)));
    if (!inner_request.has_value())
        return;

    // 3. Let realm be this's relevant realm.
    // 4. Let promise be a new promise.
    // 5. Run these substeps in parallel:
    //    1. Let requests be an empty list.
    //    2. If the optional argument request is omitted, then:
    //       1. For each requestResponse of the relevant request response list:
    //          1. Add requestResponse's request to requests.
    //    3. Else:
    //       1. Let requestResponses be the result of running Query Cache with r and options.
    //       2. For each requestResponse of requestResponses:
    //          1. Add requestResponse's request to requests.
    // NB: The UI process holds the relevant request response list, and finds the requests.
    Optional<CachedRequest> request_query;
    if (*inner_request)
        request_query = describe_cached_request(**inner_request);
    CacheQueryRequest query {
        .cache = m_request_response_list,
        .kind = CacheQueryKind::Keys,
        .request = move(request_query),
        .options = cache_query_parameters(options),
    };
    request_cache_storage_operation(HTML::principal_realm_settings_object(realm), { move(query) }, GC::create_function(GC::Heap::the(), GC::captures(promise), [](auto& promise, CacheStorageResult result) {
        // 4. Queue a task, on promise's relevant settings object's responsible event loop using the DOM manipulation
        //    task source, to perform the following steps:
        queue_cache_task(promise, GC::create_function(GC::Heap::the(), GC::captures(promise, move(result)), [](auto& promise, auto& result) {
            if (reject_with_exception_of(promise, result))
                return;
            auto& realm = WebIDL::promise_realm(promise);

            // 1. Let requestList be a list.
            // 2. For each request in requests:
            //    1. Add a new Request object associated with request and a new associated Headers object whose guard is
            //       "request" to requestList.
            // 3. Resolve promise with a frozen array created from requestList, in realm.
            auto request_list = GC::Heap::the().allocate<GC::HeapVector<JS::Value>>();
            for (auto const& item : result.template get<Vector<CachedRequestResponse>>()) {
                auto request = request_from_cached_request(item.request);
                if (!request)
                    continue;
                request_list->elements().append(Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, Fetch::Request::create(*request, Fetch::Headers::Guard::Request, DOM::AbortSignal::create())));
            }
            WebIDL::resolve_promise(promise, JS::Array::create_from(realm, request_list->elements()));
        }));
    }));

    // 6. Return promise.
}

}
