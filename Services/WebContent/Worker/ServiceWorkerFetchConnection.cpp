/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <WebContent/Worker/ServiceWorkerFetchConnection.h>
#include <WebContent/Worker/ServiceWorkerHost.h>

namespace WebWorker {

ServiceWorkerFetchConnection::ServiceWorkerFetchConnection(NonnullOwnPtr<IPC::Transport> transport, ServiceWorkerHost& host, Web::HTML::EnvironmentId client)
    : IPC::ConnectionFromClient<ServiceWorkerFetchClientEndpoint, ServiceWorkerFetchServerEndpoint>(*this, move(transport), 1)
    , m_host(host.make_weak_ptr())
    , m_client(move(client))
{
}

ServiceWorkerFetchConnection::~ServiceWorkerFetchConnection() = default;

void ServiceWorkerFetchConnection::die()
{
    if (auto host = m_host.strong_ref())
        host->fetch_connection_closed(*this);
}

void ServiceWorkerFetchConnection::dispatch_fetch_event(u64 fetch_id, Web::ServiceWorker::FetchEventRequest request)
{
    if (auto host = m_host.strong_ref())
        host->dispatch_fetch_event(*this, fetch_id, move(request));
}

void ServiceWorkerFetchConnection::allow_fetch_response_body(u64 fetch_id, u64 total_byte_count)
{
    if (auto host = m_host.strong_ref())
        host->allow_fetch_response_body(*this, fetch_id, total_byte_count);
}

void ServiceWorkerFetchConnection::cancel_fetch(u64 fetch_id)
{
    if (auto host = m_host.strong_ref())
        host->cancel_fetch(*this, fetch_id);
}

}
