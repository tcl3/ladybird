/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/WeakPtr.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibWeb/ServiceWorker/FetchEventTypes.h>
#include <LibWeb/ServiceWorker/ServiceWorkerFetchClientEndpoint.h>
#include <LibWeb/ServiceWorker/ServiceWorkerFetchServerEndpoint.h>
#include <LibWebCommon/HTML/Scripting/EnvironmentId.h>
#include <WebContent/Worker/Forward.h>

namespace WebWorker {

// The end, in a service worker's process, of the channel that the UI process set up for one service worker client,
// which carries the client's fetch events.
class ServiceWorkerFetchConnection final
    : public IPC::ConnectionFromClient<ServiceWorkerFetchClientEndpoint, ServiceWorkerFetchServerEndpoint> {
    C_OBJECT(ServiceWorkerFetchConnection);

public:
    virtual ~ServiceWorkerFetchConnection() override;

    Web::HTML::EnvironmentId const& client() const { return m_client; }

private:
    ServiceWorkerFetchConnection(NonnullOwnPtr<IPC::Transport>, ServiceWorkerHost&, Web::HTML::EnvironmentId client);

    virtual void die() override;

    virtual void dispatch_fetch_event(u64 fetch_id, Web::ServiceWorker::FetchEventRequest) override;
    virtual void allow_fetch_response_body(u64 fetch_id, u64 total_byte_count) override;
    virtual void cancel_fetch(u64 fetch_id) override;

    WeakPtr<ServiceWorkerHost> m_host;
    Web::HTML::EnvironmentId m_client;
};

}
