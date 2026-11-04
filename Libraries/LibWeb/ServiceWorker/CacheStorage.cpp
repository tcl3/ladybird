/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/Fetch/Response.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/ServiceWorker/Cache.h>
#include <LibWeb/ServiceWorker/CacheStorage.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::ServiceWorker {

GC_DEFINE_ALLOCATOR(CacheStorage);

CacheStorage::CacheStorage()
{
}

void CacheStorage::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto const& [request_id, on_result] : m_pending_operations)
        visitor.visit(on_result);
}

void CacheStorage::add_pending_operation(u64 request_id, GC::Ref<OnCacheStorageResult> on_result)
{
    m_pending_operations.set(request_id, on_result);
}

GC::Ptr<OnCacheStorageResult> CacheStorage::take_pending_operation(u64 request_id)
{
    auto on_result = m_pending_operations.take(request_id);
    if (!on_result.has_value())
        return nullptr;
    return *on_result;
}

// https://w3c.github.io/ServiceWorker/#cache-storage-match
void CacheStorage::match(JS::Realm& realm, Fetch::RequestInfo request, MultiCacheQueryOptions options, GC::Ref<WebIDL::Promise> promise)
{
    // NB: The steps of match() of the Cache interface that find r run here, before the UI process runs the rest of them
    //     on the caches of the relevant name to cache map.
    RefPtr<Fetch::Infrastructure::Request> inner_request;
    bool completed_synchronously = false;
    request.visit(
        [&](GC::Ref<Fetch::Request> const& request) {
            inner_request = request->request();
            if (inner_request->method() != "GET"sv && !options.ignore_method) {
                WebIDL::resolve_promise(promise);
                completed_synchronously = true;
            }
        },
        [&](Utf16String const& request) {
            auto request_object = Fetch::Request::create_with_settings(HTML::principal_realm_settings_object(realm), request);
            if (request_object.is_error()) {
                WebIDL::reject_promise_with_exception(promise, request_object.release_error());
                completed_synchronously = true;
                return;
            }
            inner_request = request_object.value()->request();
        });
    if (completed_synchronously)
        return;

    Optional<String> cache_name;
    if (options.cache_name.has_value())
        cache_name = options.cache_name->to_utf8();
    CacheStorageMatchRequest match {
        .request = describe_cached_request(*inner_request),
        .options = cache_query_parameters(options),
        .cache_name = move(cache_name),
    };

    // 1. If options["cacheName"] exists, then:
    //    1. Return a new promise promise and run the following substeps in parallel:
    //       1. For each cacheName → cache of the relevant name to cache map:
    //          1. If options["cacheName"] matches cacheName, then:
    //             1. Resolve promise with the result of running the algorithm specified in match(request, options)
    //                method of Cache interface with request and options (providing cache as thisArgument to the
    //                [[Call]] internal method of match(request, options).)
    //             2. Abort these steps.
    //       2. Resolve promise with undefined.
    // 2. Else:
    //    1. Let promise be a promise resolved with undefined.
    //    2. For each cacheName → cache of the relevant name to cache map:
    //       1. Set promise to the result of reacting to itself with a fulfillment handler that, when called with
    //          argument response, performs the following substeps:
    //          1. If response is not undefined, return response.
    //          2. Return the result of running the algorithm specified in match(request, options) method of Cache
    //             interface with request and options as the arguments (providing cache as thisArgument to the [[Call]]
    //             internal method of match(request, options).)
    //    3. Return promise.
    // NB: The UI process holds the relevant name to cache map, and finds the response.
    request_cache_storage_operation(HTML::principal_realm_settings_object(realm), { move(match) }, GC::create_function(GC::Heap::the(), GC::captures(promise), [](auto& promise, CacheStorageResult result) {
        queue_cache_task(promise, GC::create_function(GC::Heap::the(), GC::captures(promise, move(result)), [](auto& promise, auto& result) {
            if (reject_with_exception_of(promise, result))
                return;
            auto& realm = WebIDL::promise_realm(promise);
            auto const& responses = result.template get<Vector<CachedRequestResponse>>();
            if (responses.is_empty()) {
                WebIDL::resolve_promise(promise);
                return;
            }
            WebIDL::resolve_promise(promise, Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, response_object_from(realm, responses.first().response)));
        }));
    }));
}

// NB: Runs the substeps of has(), open(), delete() or keys() that run in parallel, in the UI process, and settles the
//     promise with what they resolve it with.
void CacheStorage::run_name_operation(CacheStorageNameOperation operation, Utf16String const& cache_name, GC::Ref<WebIDL::Promise> promise)
{
    CacheStorageNameRequest request { .operation = operation, .cache_name = cache_name.to_utf8() };
    request_cache_storage_operation(HTML::relevant_settings_object(promise->promise()), { move(request) }, GC::create_function(GC::Heap::the(), GC::captures(promise), [](auto& promise, CacheStorageResult result) {
        queue_cache_task(promise, GC::create_function(GC::Heap::the(), GC::captures(promise, move(result)), [](auto& promise, auto& result) {
            if (reject_with_exception_of(promise, result))
                return;
            auto& realm = WebIDL::promise_realm(promise);
            result.visit(
                [&](bool value) { WebIDL::resolve_promise(promise, JS::Value { value }); },
                [&](CacheId cache_id) {
                    auto cache_object = GC::Heap::the().allocate<Cache>(cache_id);
                    WebIDL::resolve_promise(promise, Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, cache_object));
                },
                [&](Vector<String> const& cache_keys) {
                    WebIDL::resolve_promise(promise, JS::Array::create_from<String>(realm, cache_keys, [&](String const& cache_key) {
                        return JS::PrimitiveString::create(realm.vm(), Utf16String::from_utf8(cache_key));
                    }));
                },
                [&](auto const&) { VERIFY_NOT_REACHED(); });
        }));
    }));
}

// https://w3c.github.io/ServiceWorker/#cache-storage-has
void CacheStorage::has(JS::Realm&, Utf16String const& cache_name, GC::Ref<WebIDL::Promise> promise)
{
    // 1. Let promise be a new promise.
    // 2. Run the following substeps in parallel:
    //    1. For each key → value of the relevant name to cache map:
    //       1. If cacheName matches key, resolve promise with true and abort these steps.
    //    2. Resolve promise with false.
    // 3. Return promise.
    run_name_operation(CacheStorageNameOperation::Has, cache_name, promise);
}

// https://w3c.github.io/ServiceWorker/#cache-storage-open
void CacheStorage::open(JS::Realm&, Utf16String const& cache_name, GC::Ref<WebIDL::Promise> promise)
{
    // 1. Let promise be a new promise.
    // 2. Run the following substeps in parallel:
    //    1. For each key → value of the relevant name to cache map:
    //       1. If cacheName matches key, then:
    //          1. Resolve promise with a new Cache object that represents value.
    //          2. Abort these steps.
    //    2. Let cache be a new request response list.
    //    3. Set the relevant name to cache map[cacheName] to cache. If this cache write operation failed due to
    //       exceeding the granted quota limit, reject promise with a "QuotaExceededError" DOMException and abort these
    //       steps.
    //    4. Resolve promise with a new Cache object that represents cache.
    // 3. Return promise.
    run_name_operation(CacheStorageNameOperation::Open, cache_name, promise);
}

// https://w3c.github.io/ServiceWorker/#cache-storage-delete
void CacheStorage::delete_(JS::Realm&, Utf16String const& cache_name, GC::Ref<WebIDL::Promise> promise)
{
    // 1. Let promise be a new promise.
    // 2. Run the following substeps in parallel:
    //    1. Let cacheExists be false.
    //    2. For each key → value of the relevant name to cache map:
    //       1. If cacheName matches key, then:
    //          1. Set cacheExists to true.
    //          2. Abort these steps.
    //    3. If cacheExists is false, then:
    //       1. Resolve promise with false.
    //       2. Abort these steps.
    //    4. Let cacheJobPromise be the result of running Delete Cache with cacheName.
    //    5. Resolve promise with cacheJobPromise.
    // 3. Return promise.
    // NOTE: After this step, the existing DOM objects (i.e. the currently referenced Cache, Request, and Response
    //       objects) should remain functional.
    run_name_operation(CacheStorageNameOperation::Delete, cache_name, promise);
}

// https://w3c.github.io/ServiceWorker/#cache-storage-keys
void CacheStorage::keys(JS::Realm&, GC::Ref<WebIDL::Promise> promise)
{
    // 1. Let promise be a new promise.
    // 2. Run the following substeps in parallel:
    //    1. Let cacheKeys be the result of getting the keys of the relevant name to cache map.
    //    NOTE: The items in the result ordered set are in the order that their corresponding entry was added to the
    //          name to cache map.
    //    2. Resolve promise with cacheKeys.
    // 3. Return promise.
    run_name_operation(CacheStorageNameOperation::Keys, {}, promise);
}

}
