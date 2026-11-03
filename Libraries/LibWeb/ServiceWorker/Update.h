/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Function.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Scripting/FetchClient.h>
#include <LibWebCommon/ServiceWorker/ServiceWorkerTypes.h>

namespace Web::ServiceWorker {

struct UpdateScriptFetch {
    GC_HOLDS_NO_CELL_EDGES;

    URL::URL script_url;
    URL::URL scope_url;
    HTML::WorkerType worker_type { HTML::WorkerType::Classic };
    ServiceWorkerUpdateViaCache update_via_cache { ServiceWorkerUpdateViaCache::Imports };
    bool force_bypass_cache { false };
};

using OnUpdateScriptFetchComplete = GC::Ref<GC::Function<void(GC::Ptr<HTML::Script>, GC::Ptr<Fetch::Infrastructure::ResponseView>, Optional<ServiceWorkerJobError>)>>;

WEB_API void fetch_the_script_of_a_new_service_worker(HTML::FetchClient const& job_client, HTML::EnvironmentSettingsObject& settings_object, UpdateScriptFetch, OnUpdateScriptFetchComplete);

}
