/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/ServiceWorkerRegistration.h>
#include <LibWeb/Export.h>

namespace Web::ServiceWorker {

// The C++ side of a ServiceWorkerRegistration, which is implemented in Rust
// (Libraries/LibWeb/Bindings/Rust/src/service_worker).
class WEB_API ServiceWorkerRegistration : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(ServiceWorkerRegistration, service_worker_registration)
};

}
