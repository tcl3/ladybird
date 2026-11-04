/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Utf16String.h>
#include <LibWebCommon/ServiceWorker/CacheStorageTypes.h>
#include <LibWebCommon/StorageAPI/StorageKey.h>
#include <LibWebView/Export.h>

namespace WebView {

// AD-HOC: Registrations and caches are keyed by the network state partition of the clients that use them as well as by
//         their storage key, so that one made under one top-level site is invisible under another.
struct ServiceWorkerPartition {
    Web::StorageAPI::StorageKey storage_key;
    Optional<Utf16String> top_level_site;
    bool has_cross_site_ancestor { false };

    bool operator==(ServiceWorkerPartition const&) const = default;
};

struct ServiceWorkerPartitionTraits : public DefaultTraits<ServiceWorkerPartition> {
    static unsigned hash(ServiceWorkerPartition const&);
};

// https://w3c.github.io/ServiceWorker/#cache-objects
// The name to cache maps of the partitions of a browsing session, and the request response lists that they name. Every
// page and worker of a partition uses the same ones.
class WEBVIEW_API ServiceWorkerCacheStorage {
public:
    Web::ServiceWorker::CacheStorageResult perform(ServiceWorkerPartition const&, Web::ServiceWorker::CacheStorageRequest const&);

private:
    using RequestResponseList = Vector<Web::ServiceWorker::CachedRequestResponse>;

    // NB: A request response list outlives its entry in the name to cache map, for the Cache objects that represent it.
    struct Cache {
        AK_ALLOC_WITH_KMALLOC;

        ServiceWorkerPartition partition;
        RequestResponseList request_response_list;
    };

    Web::ServiceWorker::CacheStorageResult perform_name_operation(ServiceWorkerPartition const&, Web::ServiceWorker::CacheStorageNameRequest const&);
    Web::ServiceWorker::CacheStorageResult match(ServiceWorkerPartition const&, Web::ServiceWorker::CacheStorageMatchRequest const&);
    Web::ServiceWorker::CacheStorageResult query(ServiceWorkerPartition const&, Web::ServiceWorker::CacheQueryRequest const&);
    Web::ServiceWorker::CacheStorageResult batch(ServiceWorkerPartition const&, Web::ServiceWorker::CacheBatchRequest const&);
    RequestResponseList* request_response_list(ServiceWorkerPartition const&, Web::ServiceWorker::CacheId);

    HashMap<ServiceWorkerPartition, OrderedHashMap<String, Web::ServiceWorker::CacheId>, ServiceWorkerPartitionTraits> m_name_to_cache_maps;
    HashMap<Web::ServiceWorker::CacheId, NonnullOwnPtr<Cache>> m_caches;
    Web::ServiceWorker::CacheId m_next_cache_id { 0 };
};

}
