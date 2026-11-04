/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Clients.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>

namespace Web::ServiceWorker {

// The C++ side of a Clients, which is implemented in Rust (Libraries/LibWeb/Bindings/Rust/src/service_worker).
class WEB_API Clients : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(Clients, clients)
};

}
