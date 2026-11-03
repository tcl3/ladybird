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
#include <LibGC/Forward.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Bindings/Client.h>
#include <LibWebCommon/Bindings/Clients.h>
#include <LibWebCommon/Bindings/Document.h>
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

// https://w3c.github.io/ServiceWorker/#update-registration-state-algorithm
enum class ServiceWorkerRegistrationSlot : u8 {
    Installing,
    Waiting,
    Active,
};

enum class ServiceWorkerLifecycleEvent : u8 {
    Install,
    Activate,
};

// https://w3c.github.io/ServiceWorker/#dfn-service-worker
struct ServiceWorkerDescriptor {
    ServiceWorkerId id { 0 };
    URL::URL script_url;
    ServiceWorkerState state { ServiceWorkerState::Parsed };
};

// https://w3c.github.io/ServiceWorker/#dfn-service-worker-registration
// https://w3c.github.io/ServiceWorker/#dfn-service-worker-client
// What a Client or WindowClient object says of the service worker client it represents. The visibility state, focus
// state and ancestor origins are those of a window client.
struct ServiceWorkerClientSnapshot {
    HTML::EnvironmentId id;
    URL::URL url;
    Bindings::ClientType type { Bindings::ClientType::Window };
    Bindings::FrameType frame_type { Bindings::FrameType::None };
    Bindings::DocumentVisibilityState visibility_state { Bindings::DocumentVisibilityState::Hidden };
    bool focused { false };
    Vector<String> ancestor_origins;
};

// The source of a message that a service worker receives: a client, or another service worker.
using ServiceWorkerMessageSource = Variant<ServiceWorkerClientSnapshot, ServiceWorkerDescriptor>;

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
    Optional<URL::URL> script_url;
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

GC_UNTRACED_CAPTURE(Web::ServiceWorker::ServiceWorkerClientSnapshot);
GC_UNTRACED_CAPTURE(Web::ServiceWorker::ServiceWorkerDescriptor);
GC_UNTRACED_CAPTURE(Web::ServiceWorker::ServiceWorkerRegistrationSnapshot);

#include <LibWebCommon/ServiceWorker/ServiceWorkerTypesCodecs.h>
