/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/ServiceWorker.h>
#include <LibWeb/Export.h>

namespace Web::ServiceWorker {

// The C++ side of a ServiceWorker, which is implemented in Rust
// (Libraries/LibWeb/Bindings/Rust/src/service_worker).
class WEB_API ServiceWorker : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(ServiceWorker, service_worker)
};

}
