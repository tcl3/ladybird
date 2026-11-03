/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/ServiceWorkerContainer.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/ServiceWorker/ServiceWorkerTypes.h>

namespace Web::ServiceWorker {

// https://w3c.github.io/ServiceWorker/#serviceworkercontainer-interface
// The C++ side of a ServiceWorkerContainer, which is implemented in Rust (Libraries/LibWeb/Bindings/Rust/src/service_worker).
class WEB_API ServiceWorkerContainer : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(ServiceWorkerContainer, service_worker_container)

public:
    [[nodiscard]] static GC::Ref<ServiceWorkerContainer> create(JS::Realm&);
};

WEB_API void service_worker_job_settled(ServiceWorkerRequestId, ServiceWorkerJobResult);
WEB_API void service_worker_registration_lookup_finished(ServiceWorkerRequestId, Vector<ServiceWorkerRegistrationSnapshot>);
WEB_API void service_worker_registration_state_changed(ServiceWorkerRegistrationId, ServiceWorkerRegistrationSlot target, Optional<ServiceWorkerDescriptor> source);
WEB_API void service_worker_state_changed(ServiceWorkerDescriptor);
WEB_API void service_worker_update_found(ServiceWorkerRegistrationId);
WEB_API void service_worker_ready(ServiceWorkerRequestId, ServiceWorkerRegistrationSnapshot);

}
