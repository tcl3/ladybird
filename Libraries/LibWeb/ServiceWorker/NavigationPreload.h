/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/NonnullRefPtr.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::ServiceWorker {

// https://w3c.github.io/ServiceWorker/#on-fetch-request-algorithm
// Starts the navigation preload of a navigation request in the realm of the service worker's settings object, which
// handles the request's fetch event, and returns preloadResponse.
WEB_API GC::Ref<WebIDL::Promise> start_navigation_preload(HTML::EnvironmentSettingsObject&, Fetch::Infrastructure::Request const&, ByteString const& header_value);

}
