/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/TransportHandle.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/ServiceWorker/FetchEventTypes.h>
#include <LibWeb/ServiceWorker/HandleFetch.h>
#include <LibWeb/ServiceWorker/ServiceWorkerFetchClientEndpoint.h>
#include <LibWeb/ServiceWorker/ServiceWorkerFetchServerEndpoint.h>
#include <LibWebCommon/HTML/Scripting/EnvironmentId.h>
#include <LibWebCommon/ServiceWorker/ServiceWorkerTypes.h>

namespace Web::ServiceWorker {

// The end, in a service worker client's process, of the channel to the process of the client's active service
// worker, which the UI process sets up for one client. The client's fetch events go over it.
class FetchChannel final
    : public IPC::ConnectionToServer<ServiceWorkerFetchClientEndpoint, ServiceWorkerFetchServerEndpoint>
    , public ServiceWorkerFetchClientEndpoint {
    C_OBJECT_ABSTRACT(FetchChannel);

public:
    FetchChannel(NonnullOwnPtr<IPC::Transport>, HTML::EnvironmentId client, ServiceWorkerId);
    virtual ~FetchChannel() override;

    HTML::EnvironmentId const& client() const { return m_client; }
    ServiceWorkerId service_worker() const { return m_service_worker; }

private:
    virtual void die() override;

    virtual void fetch_event_settled(u64 fetch_id, FetchEventOutcome, Optional<FetchEventResponse>) override;
    virtual void fetch_response_body_data(u64 fetch_id, ByteBuffer) override;
    virtual void fetch_response_body_end(u64 fetch_id, bool succeeded) override;

    HTML::EnvironmentId m_client;
    ServiceWorkerId m_service_worker { 0 };
};

// https://w3c.github.io/ServiceWorker/#create-fetch-event-and-dispatch
// Dispatches a fetch event for the request at the client's active service worker, over the client's channel to the
// worker's process. The client is the request's reserved client for a non-subresource request, and its client
// otherwise.
WEB_API void dispatch_fetch_event(Page&, HTML::EnvironmentId const& client, ServiceWorkerDescriptor const& active_worker, Fetch::Infrastructure::Request const&, HandleFetchCallback);

// The UI process's answer to a client's request for a channel to its active service worker's process.
WEB_API void service_worker_fetch_channel_ready(ServiceWorkerRequestId, Optional<IPC::TransportHandle>);

// For an agent that goes away, whose waiting fetch events would keep roots beyond the end of its heap.
void forget_fetch_channels_of_this_agent();

}
