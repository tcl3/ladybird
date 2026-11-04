/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <LibGC/Function.h>
#include <LibWeb/Bindings/CacheStorage.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Fetch/Request.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWebCommon/ServiceWorker/CacheStorageTypes.h>

namespace Web::ServiceWorker {

using CacheQueryOptions = Bindings::CacheQueryOptions;
using MultiCacheQueryOptions = Bindings::MultiCacheQueryOptions;

// https://w3c.github.io/ServiceWorker/#cache-interface
class Cache : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(Cache, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(Cache);

public:
    void match(JS::Realm&, Fetch::RequestInfo, CacheQueryOptions, GC::Ref<WebIDL::Promise>);
    void match_all(JS::Realm&, Optional<Fetch::RequestInfo>, CacheQueryOptions, GC::Ref<WebIDL::Promise>);
    void add(JS::Realm&, Fetch::RequestInfo, GC::Ref<WebIDL::Promise>);
    void add_all(JS::Realm&, ReadonlySpan<Fetch::RequestInfo>, GC::Ref<WebIDL::Promise>);
    void put(JS::Realm&, Fetch::RequestInfo, GC::Ref<Fetch::Response>, GC::Ref<WebIDL::Promise>);
    void delete_(JS::Realm&, Fetch::RequestInfo, CacheQueryOptions, GC::Ref<WebIDL::Promise>);
    void keys(JS::Realm&, Optional<Fetch::RequestInfo>, CacheQueryOptions, GC::Ref<WebIDL::Promise>);

private:
    friend class CacheStorage;

    explicit Cache(CacheId);

    void run_batch_cache_operations(Vector<CacheBatchOperationDescriptor>, GC::Ref<WebIDL::Promise>);

    // https://w3c.github.io/ServiceWorker/#dfn-relevant-request-response-list
    // NB: The UI process holds the request response list that the object represents.
    CacheId m_request_response_list { 0 };
};

// What the UI process stores of a request and of a response, and the options that a query passes it.
CachedRequest describe_cached_request(Fetch::Infrastructure::Request const&);
CachedResponse describe_cached_response(Fetch::Infrastructure::ResponseView const&);
CacheQueryParameters cache_query_parameters(CacheQueryOptions const&);

// The request and the Response object for a request and a response that the UI process stored. The request is null if
// the description is not of one, which a process that the UI process does not trust can have put in the cache.
RefPtr<Fetch::Infrastructure::Request> request_from_cached_request(CachedRequest const&);
GC::Ref<Fetch::Response> response_object_from(JS::Realm&, CachedResponse const&);

// Asks the UI process to run the steps of a method of a CacheStorage or Cache object for the settings object, and calls
// back with its answer.
using OnCacheStorageResult = GC::Function<void(CacheStorageResult)>;
void request_cache_storage_operation(HTML::EnvironmentSettingsObject&, CacheStorageRequest, GC::Ref<OnCacheStorageResult>);

// For an agent that goes away, whose weak references to its CacheStorage objects would outlive its heap.
void forget_cache_storage_operations_of_this_agent();
WEB_API void cache_storage_operation_finished(u64 request_id, CacheStorageResult);

// Queues a task on the promise's relevant settings object's responsible event loop, using the DOM manipulation task
// source, to run the steps.
void queue_cache_task(GC::Ref<WebIDL::Promise>, GC::Ref<GC::Function<void()>> steps);

// Rejects the promise with the exception that the UI process's answer is, if it is one, and returns whether it did.
bool reject_with_exception_of(GC::Ref<WebIDL::Promise>, CacheStorageResult const&);

}
