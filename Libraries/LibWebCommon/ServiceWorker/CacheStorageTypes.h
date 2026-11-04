/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGC/Forward.h>
#include <LibHTTP/Cache/CacheMode.h>
#include <LibHTTP/Header.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Fetch/Infrastructure/HTTP/RequestReferrer.h>
#include <LibWebCommon/Fetch/Infrastructure/RedirectTaint.h>
#include <LibWebCommon/ReferrerPolicy/ReferrerPolicy.h>

namespace Web::ServiceWorker {

// https://w3c.github.io/ServiceWorker/#dfn-request-response-list
// A request response list, which the UI process holds, and which Cache objects in every process that can use it name
// by this id.
using CacheId = u64;

// https://fetch.spec.whatwg.org/#concept-request
// A request of a request response list. The UI process reads its method, URL list and header list. The other fields
// are the underlying values of the enums that LibWeb defines.
struct CachedRequest {
    ByteString method;
    Vector<URL::URL> url_list;
    Vector<HTTP::Header> header_list;
    Optional<u8> destination;
    u8 mode { 0 };
    u8 credentials_mode { 0 };
    HTTP::CacheMode cache_mode { HTTP::CacheMode::Default };
    u8 redirect_mode { 0 };
    Fetch::Infrastructure::RequestReferrerType referrer;
    ReferrerPolicy::ReferrerPolicy referrer_policy { ReferrerPolicy::ReferrerPolicy::EmptyString };
    String integrity_metadata;
    bool keepalive { false };
    bool reload_navigation { false };
    bool history_navigation { false };
};

// https://fetch.spec.whatwg.org/#concept-response
// A response of a request response list, with its type and the fields of its internal response, and its body's bytes.
// The UI process reads its filtered header list: the response's header list, which the filter of a filtered response
// limits.
struct CachedResponse {
    u8 type { 0 };
    Vector<HTTP::Header> filtered_header_list;
    u16 status { 200 };
    ByteString status_message;
    Vector<HTTP::Header> header_list;
    Vector<URL::URL> url_list;
    Vector<ByteString> cors_exposed_header_name_list;
    bool range_requested { false };
    bool request_includes_credentials { true };
    bool timing_allow_passed { false };
    Fetch::Infrastructure::RedirectTaint redirect_taint { Fetch::Infrastructure::RedirectTaint::SameOrigin };
    Optional<ByteBuffer> body;
};

// https://w3c.github.io/ServiceWorker/#dictdef-cachequeryoptions
struct CacheQueryParameters {
    bool ignore_search { false };
    bool ignore_method { false };
    bool ignore_vary { false };
};

// An item of a request response list.
struct CachedRequestResponse {
    CachedRequest request;
    CachedResponse response;
};

// https://w3c.github.io/ServiceWorker/#dfn-cache-batch-operation
enum class CacheBatchOperationType : u8 {
    Delete,
    Put,
};

struct CacheBatchOperationDescriptor {
    CacheBatchOperationType type { CacheBatchOperationType::Delete };
    CachedRequest request;
    Optional<CachedResponse> response;
    CacheQueryParameters options;
};

// The steps of a CacheStorage method that run in parallel on the name to cache map.
enum class CacheStorageNameOperation : u8 {
    Has,
    Open,
    Delete,
    Keys,
};

struct CacheStorageNameRequest {
    CacheStorageNameOperation operation { CacheStorageNameOperation::Has };
    String cache_name;
};

// https://w3c.github.io/ServiceWorker/#cache-storage-match
struct CacheStorageMatchRequest {
    CachedRequest request;
    CacheQueryParameters options;
    Optional<String> cache_name;
};

// The steps of matchAll() and keys() of a Cache object that run in parallel on its request response list.
enum class CacheQueryKind : u8 {
    MatchAll,
    Keys,
};

struct CacheQueryRequest {
    CacheId cache { 0 };
    CacheQueryKind kind { CacheQueryKind::MatchAll };
    Optional<CachedRequest> request;
    CacheQueryParameters options;
};

// https://w3c.github.io/ServiceWorker/#batch-cache-operations-algorithm
struct CacheBatchRequest {
    CacheId cache { 0 };
    Vector<CacheBatchOperationDescriptor> operations;
};

// The steps of a method of a CacheStorage or Cache object that the UI process runs.
struct CacheStorageRequest {
    Variant<CacheStorageNameRequest, CacheStorageMatchRequest, CacheQueryRequest, CacheBatchRequest> operation;
};

// The exceptions that the steps that run in the UI process throw.
enum class CacheStorageError : u8 {
    TypeError,
    InvalidStateError,
    SecurityError,
};

// What the UI process answers: the items that match a query, whether a cache existed or a batch removed an item, the
// id of a cache that was opened, the names of the caches, or an exception.
using CacheStorageResult = Variant<Vector<CachedRequestResponse>, bool, CacheId, Vector<String>, CacheStorageError>;

}

GC_UNTRACED_CAPTURE(Web::ServiceWorker::CachedRequestResponse);
GC_UNTRACED_CAPTURE(Web::ServiceWorker::CacheBatchOperationDescriptor);

#include <LibWebCommon/ServiceWorker/CacheStorageTypesCodecs.h>
