/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/ServiceWorker/ServiceWorkerTypes.h>

namespace Web::ServiceWorker {

// Handle Fetch returns null, for the request to go to the network, or a response.
using HandleFetchCallback = GC::Ref<GC::Function<void(RefPtr<Fetch::Infrastructure::Response>)>>;

// https://w3c.github.io/ServiceWorker/#on-fetch-request-algorithm
WEB_API void handle_fetch(Page&, NonnullRefPtr<Fetch::Infrastructure::Request>, GC::Ref<Fetch::Infrastructure::FetchController>, bool use_high_res_performance_timers, HandleFetchCallback);

// The UI process's answer to a request to match a reserved client's request with a registration.
WEB_API void service_worker_match_finished(ServiceWorkerRequestId, Optional<ServiceWorkerControllerMatch>);

// For an agent that goes away, whose waiting fetches would keep roots beyond the end of its heap.
void forget_pending_matches_of_this_agent();

}
