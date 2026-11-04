/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/ServiceWorker/Cache.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::ServiceWorker {

// https://w3c.github.io/ServiceWorker/#cachestorage-interface
// NB: The UI process holds the relevant name to cache map, which every page and worker of a storage key share.
class CacheStorage : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(CacheStorage, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(CacheStorage);

public:
    void match(JS::Realm&, Fetch::RequestInfo, MultiCacheQueryOptions, GC::Ref<WebIDL::Promise>);
    void has(JS::Realm&, Utf16String const& cache_name, GC::Ref<WebIDL::Promise>);
    void open(JS::Realm&, Utf16String const& cache_name, GC::Ref<WebIDL::Promise>);
    void delete_(JS::Realm&, Utf16String const& cache_name, GC::Ref<WebIDL::Promise>);
    void keys(JS::Realm&, GC::Ref<WebIDL::Promise>);

    // NB: The steps that run once the UI process answers an operation of this object's global, which this object keeps
    //     alive until then.
    void add_pending_operation(u64 request_id, GC::Ref<OnCacheStorageResult>);
    GC::Ptr<OnCacheStorageResult> take_pending_operation(u64 request_id);

private:
    CacheStorage();

    virtual void visit_edges(GC::Cell::Visitor&) override;

    void run_name_operation(CacheStorageNameOperation, Utf16String const& cache_name, GC::Ref<WebIDL::Promise>);

    HashMap<u64, GC::Ref<OnCacheStorageResult>> m_pending_operations;
};

}
