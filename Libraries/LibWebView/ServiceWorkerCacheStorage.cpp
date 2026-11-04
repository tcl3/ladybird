/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibHTTP/HeaderList.h>
#include <LibWebView/ServiceWorkerCacheStorage.h>

namespace WebView {

using namespace Web::ServiceWorker;

unsigned ServiceWorkerPartitionTraits::hash(ServiceWorkerPartition const& partition)
{
    auto hash = Traits<Web::StorageAPI::StorageKey>::hash(partition.storage_key);
    if (partition.top_level_site.has_value())
        hash = pair_int_hash(hash, partition.top_level_site->hash());
    return pair_int_hash(hash, static_cast<unsigned>(partition.has_cross_site_ancestor));
}

// NB: The requests and responses come from the processes of pages and workers, which the UI process does not trust to
//     describe them as a fetch would have.
static bool is_well_formed(CachedRequest const& request)
{
    return !request.url_list.is_empty();
}

// https://fetch.spec.whatwg.org/#concept-request-url
static URL::URL const& url_of(CachedRequest const& request)
{
    // A request's URL is the first URL in request's URL list.
    return request.url_list.first();
}

// https://w3c.github.io/ServiceWorker/#request-matches-cached-item-algorithm
static bool request_matches_cached_item(CachedRequest const& request_query, CachedRequest const& request, CachedResponse const* response, CacheQueryParameters options)
{
    // 1. If options["ignoreMethod"] is false and request's method is not `GET`, return false.
    if (!options.ignore_method && request.method != "GET"sv)
        return false;

    // 2. Let queryURL be requestQuery's url.
    auto query_url = url_of(request_query);

    // 3. Let cachedURL be request's url.
    auto cached_url = url_of(request);

    // 4. If options["ignoreSearch"] is true, then:
    if (options.ignore_search) {
        // 1. Set cachedURL's query to the empty string.
        cached_url.set_query(""sv);

        // 2. Set queryURL's query to the empty string.
        query_url.set_query(""sv);
    }

    // 5. If queryURL does not equal cachedURL with the exclude fragment flag set, then return false.
    if (!query_url.equals(cached_url, URL::ExcludeFragment::Yes))
        return false;

    // 6. If response is null, options["ignoreVary"] is true, or response's header list does not contain `Vary`, then
    //    return true.
    if (!response || options.ignore_vary)
        return true;
    auto response_header_list = HTTP::HeaderList::create(response->filtered_header_list);
    if (!response_header_list->contains("Vary"sv))
        return true;

    // 7. Let fieldValues be the list containing the elements corresponding to the field-values of the Vary header for
    //    the value of the header with name `Vary`.
    // 8. For each fieldValue in fieldValues:
    auto request_header_list = HTTP::HeaderList::create(request.header_list);
    auto request_query_header_list = HTTP::HeaderList::create(request_query.header_list);
    bool matches = true;
    response_header_list->for_each_vary_header([&](StringView field_value) {
        // 1. If fieldValue matches "*", or the combined value given fieldValue and request's header list does not match
        //    the combined value given fieldValue and requestQuery's header list, then return false.
        if (field_value == "*"sv || request_header_list->get(field_value) != request_query_header_list->get(field_value)) {
            matches = false;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });

    // 9. Return true.
    return matches;
}

// https://w3c.github.io/ServiceWorker/#query-cache-algorithm
// NB: This returns the indices of the items in storage that match, from which a caller copies what it needs.
static Vector<size_t> query_cache(CachedRequest const& request_query, CacheQueryParameters options, Vector<CachedRequestResponse> const& storage)
{
    // 1. Let resultList be an empty list.
    Vector<size_t> result_list;

    // 5. For each requestResponse of storage:
    for (size_t index = 0; index < storage.size(); ++index) {
        // 1. Let cachedRequest be requestResponse's request.
        // 2. Let cachedResponse be requestResponse's response.
        auto const& [cached_request, cached_response] = storage[index];

        // 3. If Request Matches Cached Item with requestQuery, cachedRequest, cachedResponse, and options returns true,
        //    then:
        //    1. Let requestCopy be a copy of cachedRequest.
        //    2. Let responseCopy be a copy of cachedResponse.
        //    3. Add requestCopy/responseCopy to resultList.
        if (request_matches_cached_item(request_query, cached_request, &cached_response, options))
            result_list.append(index);
    }

    // 6. Return resultList.
    return result_list;
}

CacheStorageResult ServiceWorkerCacheStorage::perform(ServiceWorkerPartition const& partition, CacheStorageRequest const& request)
{
    return request.operation.visit(
        [&](CacheStorageNameRequest const& request) { return perform_name_operation(partition, request); },
        [&](CacheStorageMatchRequest const& request) { return match(partition, request); },
        [&](CacheQueryRequest const& request) { return query(partition, request); },
        [&](CacheBatchRequest const& request) { return batch(partition, request); });
}

ServiceWorkerCacheStorage::RequestResponseList* ServiceWorkerCacheStorage::request_response_list(ServiceWorkerPartition const& partition, CacheId cache_id)
{
    // NB: A process can name only the caches of its own partitions.
    auto cache = m_caches.get(cache_id);
    if (!cache.has_value() || (*cache)->partition != partition)
        return nullptr;
    return &(*cache)->request_response_list;
}

CacheStorageResult ServiceWorkerCacheStorage::perform_name_operation(ServiceWorkerPartition const& partition, CacheStorageNameRequest const& request)
{
    auto& name_to_cache_map = m_name_to_cache_maps.ensure(partition);

    switch (request.operation) {
    // https://w3c.github.io/ServiceWorker/#cache-storage-has
    case CacheStorageNameOperation::Has:
        // 2.1. For each key → value of the relevant name to cache map:
        //      1. If cacheName matches key, resolve promise with true and abort these steps.
        // 2.2. Resolve promise with false.
        return name_to_cache_map.contains(request.cache_name);

    // https://w3c.github.io/ServiceWorker/#cache-storage-open
    case CacheStorageNameOperation::Open: {
        // 2.1. For each key → value of the relevant name to cache map:
        //      1. If cacheName matches key, then:
        //         1. Resolve promise with a new Cache object that represents value.
        //         2. Abort these steps.
        if (auto cache_id = name_to_cache_map.get(request.cache_name); cache_id.has_value())
            return *cache_id;

        // 2.2. Let cache be a new request response list.
        // 2.3. Set the relevant name to cache map[cacheName] to cache. If this cache write operation failed due to
        //      exceeding the granted quota limit, reject promise with a "QuotaExceededError" DOMException and abort
        //      these steps.
        // FIXME: Limit the size of the caches.
        auto cache_id = ++m_next_cache_id;
        m_caches.set(cache_id, make<Cache>(partition, RequestResponseList {}));
        name_to_cache_map.set(request.cache_name, cache_id);

        // 2.4. Resolve promise with a new Cache object that represents cache.
        return cache_id;
    }

    // https://w3c.github.io/ServiceWorker/#cache-storage-delete
    case CacheStorageNameOperation::Delete:
        // 2.1. Let cacheExists be false.
        // 2.2. For each key → value of the relevant name to cache map:
        //      1. If cacheName matches key, then:
        //         1. Set cacheExists to true.
        //         2. Abort these steps.
        // 2.3. If cacheExists is false, then:
        //      1. Resolve promise with false.
        //      2. Abort these steps.
        // 2.4. Let cacheJobPromise be the result of running Delete Cache with cacheName.
        // NB: The existing Cache objects keep representing the request response list, which outlives its name.
        return name_to_cache_map.remove(request.cache_name);

    // https://w3c.github.io/ServiceWorker/#cache-storage-keys
    case CacheStorageNameOperation::Keys:
        // 2.1. Let cacheKeys be the result of getting the keys of the relevant name to cache map.
        // NB: The items in the result ordered set are in the order that their corresponding entry was added to the name
        //     to cache map.
        // 2.2. Resolve promise with cacheKeys.
        return name_to_cache_map.keys();
    }
    VERIFY_NOT_REACHED();
}

// https://w3c.github.io/ServiceWorker/#cache-matchall
static Vector<CachedRequestResponse> match_all(Vector<CachedRequestResponse> const& request_response_list, Optional<CachedRequest> const& request, CacheQueryParameters options)
{
    // 1. Let responses be an empty list.
    Vector<CachedRequestResponse> responses;

    // 2. If the optional argument request is omitted, then:
    if (!request.has_value()) {
        // 1. For each requestResponse of the relevant request response list:
        //    1. Add a copy of requestResponse's response to responses.
        responses.extend(request_response_list);
        return responses;
    }

    // NB: The client's process returns an empty array for a Request object whose method is not `GET`, unless
    //     options["ignoreMethod"] is true.
    if (request->method != "GET"sv && !options.ignore_method)
        return responses;

    // 3. Else:
    //    1. Let requestResponses be the result of running Query Cache with r and options.
    //    2. For each requestResponse of requestResponses:
    //       1. Add a copy of requestResponse's response to responses.
    for (auto index : query_cache(*request, options, request_response_list))
        responses.append(request_response_list[index]);
    return responses;
}

// https://w3c.github.io/ServiceWorker/#cache-storage-match
CacheStorageResult ServiceWorkerCacheStorage::match(ServiceWorkerPartition const& partition, CacheStorageMatchRequest const& request)
{
    if (!is_well_formed(request.request))
        return CacheStorageError::TypeError;
    auto& name_to_cache_map = m_name_to_cache_maps.ensure(partition);

    // NB: Cache.match() resolves with the first of the responses that matchAll() finds.
    auto first_match = [&](CacheId cache_id) -> Vector<CachedRequestResponse> {
        auto responses = match_all(m_caches.get(cache_id).value()->request_response_list, request.request, request.options);
        if (responses.size() > 1)
            responses.shrink(1);
        return responses;
    };

    // 1. If options["cacheName"] exists, then:
    if (request.cache_name.has_value()) {
        // 1. Return a new promise promise and run the following substeps in parallel:
        //    1. For each cacheName → cache of the relevant name to cache map:
        //       1. If options["cacheName"] matches cacheName, then:
        //          1. Resolve promise with the result of running the algorithm specified in match(request, options)
        //             method of Cache interface with request and options (providing cache as thisArgument to the
        //             [[Call]] internal method of match(request, options).)
        //          2. Abort these steps.
        if (auto cache_id = name_to_cache_map.get(*request.cache_name); cache_id.has_value())
            return first_match(*cache_id);

        //    2. Resolve promise with undefined.
        return Vector<CachedRequestResponse> {};
    }

    // 2. Else:
    //    1. Let promise be a promise resolved with undefined.
    //    2. For each cacheName → cache of the relevant name to cache map:
    //       1. Set promise to the result of reacting to itself with a fulfillment handler that, when called with argument
    //          response, performs the following substeps:
    //          1. If response is not undefined, return response.
    //          2. Return the result of running the algorithm specified in match(request, options) method of Cache
    //             interface with request and options as the arguments (providing cache as thisArgument to the [[Call]]
    //             internal method of match(request, options).)
    for (auto const& [cache_name, cache_id] : name_to_cache_map) {
        if (auto response = first_match(cache_id); !response.is_empty())
            return response;
    }

    //    3. Return promise.
    return Vector<CachedRequestResponse> {};
}

CacheStorageResult ServiceWorkerCacheStorage::query(ServiceWorkerPartition const& partition, CacheQueryRequest const& request)
{
    auto* request_response_list = this->request_response_list(partition, request.cache);
    if (!request_response_list || (request.request.has_value() && !is_well_formed(*request.request)))
        return CacheStorageError::TypeError;

    auto items = match_all(*request_response_list, request.request, request.options);

    // https://w3c.github.io/ServiceWorker/#cache-keys
    // NB: keys() resolves with the requests alone.
    if (request.kind == CacheQueryKind::Keys) {
        for (auto& item : items)
            item.response.body = {};
    }
    return items;
}

// https://w3c.github.io/ServiceWorker/#batch-cache-operations-algorithm
CacheStorageResult ServiceWorkerCacheStorage::batch(ServiceWorkerPartition const& partition, CacheBatchRequest const& request)
{
    // 1. Let cache be the relevant request response list.
    auto* cache = request_response_list(partition, request.cache);
    if (!cache)
        return CacheStorageError::TypeError;

    // 2. Let backupCache be a new request response list that is a copy of cache.
    // 3. Let addedItems be an empty list.
    // 4. Try running the following substeps atomically:
    // NB: Whether an operation throws depends on the operations before it alone, not on cache. So these steps check
    //     every operation before any of them changes cache, which then has nothing to undo.
    Vector<CacheBatchOperationDescriptor const*> added_items;
    for (auto const& operation : request.operations) {
        if (!is_well_formed(operation.request))
            return CacheStorageError::TypeError;

        // 2.1. If operation's type matches neither "delete" nor "put", throw a TypeError.
        // 2.2. If operation's type matches "delete" and operation's response is not null, throw a TypeError.
        if (operation.type == CacheBatchOperationType::Delete && operation.response.has_value())
            return CacheStorageError::TypeError;

        // 2.3. If the result of running Query Cache with operation's request, operation's options, and addedItems is
        //      not empty, throw an "InvalidStateError" DOMException.
        for (auto const* added_item : added_items) {
            if (request_matches_cached_item(operation.request, added_item->request, &*added_item->response, operation.options))
                return CacheStorageError::InvalidStateError;
        }

        if (operation.type == CacheBatchOperationType::Put) {
            // 2.6.1. If operation's response is null, throw a TypeError.
            if (!operation.response.has_value())
                return CacheStorageError::TypeError;

            // 2.6.3. If r's url's scheme is not one of "http" and "https", throw a TypeError.
            // 2.6.4. If r's method is not `GET`, throw a TypeError.
            if (!url_of(operation.request).scheme().is_one_of("http"sv, "https"sv) || operation.request.method != "GET"sv)
                return CacheStorageError::TypeError;

            // 2.6.10. Append operation's request/operation's response to addedItems.
            added_items.append(&operation);
        }
    }

    // 1. Let resultList be an empty list.
    // NB: Cache.delete() needs to know only whether there were any results.
    bool removed_any_items = false;
    auto remove_items = [&](Vector<size_t> const& indices) {
        for (size_t i = indices.size(); i > 0; --i)
            cache->remove(indices[i - 1]);
        removed_any_items |= !indices.is_empty();
    };

    // 2. For each operation in operations:
    for (auto const& operation : request.operations) {
        // 4. Let requestResponses be an empty list.
        // 5. If operation's type matches "delete", then:
        if (operation.type == CacheBatchOperationType::Delete) {
            // 1. Set requestResponses to the result of running Query Cache with operation's request and operation's
            //    options.
            // 2. For each requestResponse in requestResponses:
            //    1. Remove the item whose value matches requestResponse from cache.
            remove_items(query_cache(operation.request, operation.options, *cache));
        }
        // 6. Else if operation's type matches "put", then:
        else {
            // 5. If operation's options is not null, throw a TypeError.
            // 6. Set requestResponses to the result of running Query Cache with operation's request.
            // 7. For each requestResponse of requestResponses:
            //    1. Remove the item whose value matches requestResponse from cache.
            remove_items(query_cache(operation.request, {}, *cache));

            // 8. Append operation's request/operation's response to cache.
            // FIXME: 9. If the cache write operation in the previous two steps failed due to exceeding the granted quota
            //           limit, throw a "QuotaExceededError" DOMException.
            cache->append(CachedRequestResponse {
                .request = operation.request,
                .response = *operation.response,
            });
        }

        // 7. Append operation's request/operation's response to resultList.
    }

    // 3. Return resultList.
    return removed_any_items;
}

}
