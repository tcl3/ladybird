/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Types.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Bindings/ServiceWorker.h>
#include <LibWebCommon/Bindings/ServiceWorkerRegistration.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/HTML/Scripting/EnvironmentId.h>
#include <LibWebCommon/HTML/Scripting/SerializedEnvironmentSettingsObject.h>
#include <LibWebCommon/HTML/WorkerTypes.h>

namespace Web::ServiceWorker {

using ServiceWorkerRegistrationId = u64;
using ServiceWorkerId = u64;
using ServiceWorkerRequestId = u64;

using ServiceWorkerState = Bindings::ServiceWorkerState;
using ServiceWorkerUpdateViaCache = Bindings::ServiceWorkerUpdateViaCache;

// https://w3c.github.io/ServiceWorker/#dfn-job-type
enum class ServiceWorkerJobType : u8 {
    Register,
    Update,
    Unregister,
};

enum class ServiceWorkerJobError : u8 {
    TypeError,
    SecurityError,
};

// https://w3c.github.io/ServiceWorker/#dfn-service-worker
struct ServiceWorkerDescriptor {
    ServiceWorkerId id { 0 };
    URL::URL script_url;
    ServiceWorkerState state { ServiceWorkerState::Parsed };
};

// https://w3c.github.io/ServiceWorker/#dfn-service-worker-registration
struct ServiceWorkerRegistrationSnapshot {
    ServiceWorkerRegistrationId id { 0 };
    URL::URL scope_url;
    ServiceWorkerUpdateViaCache update_via_cache { ServiceWorkerUpdateViaCache::Imports };
    Optional<ServiceWorkerDescriptor> installing_worker;
    Optional<ServiceWorkerDescriptor> waiting_worker;
    Optional<ServiceWorkerDescriptor> active_worker;
};

// https://w3c.github.io/ServiceWorker/#dfn-job
struct ServiceWorkerJobRequest {
    ServiceWorkerRequestId request_id { 0 };
    ServiceWorkerJobType job_type { ServiceWorkerJobType::Register };
    HTML::SerializedEnvironmentSettingsObject client;
    URL::URL scope_url;
    URL::URL script_url;
    HTML::WorkerType worker_type { HTML::WorkerType::Classic };
    ServiceWorkerUpdateViaCache update_via_cache { ServiceWorkerUpdateViaCache::Imports };
    bool client_has_dispatched_dom_content_loaded { false };
};

using ServiceWorkerJobResult = Variant<ServiceWorkerRegistrationSnapshot, bool, ServiceWorkerJobError>;

struct ServiceWorkerScriptFetchParameters {
    HTML::SerializedEnvironmentSettingsObject job_client;
    bool force_bypass_cache { false };
};

// https://w3c.github.io/ServiceWorker/#run-service-worker-algorithm
struct ServiceWorkerStartRequest {
    ServiceWorkerDescriptor worker;
    HTML::WorkerType worker_type { HTML::WorkerType::Classic };
    ServiceWorkerRegistrationSnapshot registration;

    // https://w3c.github.io/ServiceWorker/#setup-serviceworkerglobalscope
    HTML::EnvironmentId environment_id;
    URL::Origin origin;
    bool has_cross_site_ancestor { false };
    Optional<URL::Origin> top_level_origin;

    ServiceWorkerScriptFetchParameters script_fetch;
};

using ServiceWorkerScriptFetchResult = Optional<ServiceWorkerJobError>;

}

#include <LibWebCommon/ServiceWorker/ServiceWorkerTypesCodecs.h>
