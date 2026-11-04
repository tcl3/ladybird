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
#include <LibWebCommon/HTML/SerializationRecords.h>
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
WEB_API void service_worker_message_to_client(HTML::EnvironmentId const& client_id, ServiceWorkerDescriptor source, URL::Origin const&, HTML::SerializedTransferRecord);
WEB_API void service_worker_controller_changed(HTML::EnvironmentId const& client_id, ServiceWorkerDescriptor controller);
WEB_API void enable_client_message_queue(HTML::EnvironmentSettingsObject&);
WEB_API GC::Ref<ServiceWorker> service_worker_object(JS::Realm&, ServiceWorkerDescriptor const&);
WEB_API GC::Ref<ServiceWorkerRegistration> service_worker_registration_object(JS::Realm&, ServiceWorkerRegistrationSnapshot const&);

// Dispatches the message event of a message that the service worker received, and returns the id of the event in the
// worker's set of extended events, or 0 if the event is not in the set. The callback is called with the context
// whenever the set becomes empty or stops being empty.
WEB_API u64 dispatch_message_event(ServiceWorkerGlobalScope&, ServiceWorkerMessageSource const&, URL::Origin const&, HTML::SerializedTransferRecord, void* ctx, void (*on_pending_events_changed)(void*, bool has_pending_events));
WEB_API void time_out_extended_event(u64 event_id);

WEB_API ServiceWorkerId service_worker_id(ServiceWorker const&);

WEB_API GC::Ref<Clients> create_clients(JS::Realm&);
WEB_API void service_worker_clients_matched(ServiceWorkerRequestId, Vector<ServiceWorkerClientSnapshot> const&);
WEB_API void service_worker_claim_finished(ServiceWorkerRequestId, bool succeeded);
WEB_API Vector<ServiceWorkerClientSnapshot> service_worker_client_snapshots(Vector<HTML::EnvironmentId> const&);
WEB_API void navigation_preload_settled(ServiceWorkerRequestId, Optional<ServiceWorkerNavigationPreloadState> const&);

// https://w3c.github.io/ServiceWorker/#client-navigate
// The UI process's answer to WindowClient.navigate(): an error to reject the promise with, or the window client to
// resolve it with, or neither for null.
WEB_API void service_worker_client_navigated(ServiceWorkerRequestId, Optional<ServiceWorkerJobError>, Optional<ServiceWorkerClientSnapshot> const&);

// Runs the steps of WindowClient.navigate() that the window client's process runs, for the window client with the id.
// Calls back with an error to reject the promise with, or with the window client to resolve it with, or with neither
// once the navigation started.
using WindowClientNavigationCallback = GC::Ref<GC::Function<void(Optional<ServiceWorkerJobError>, Optional<ServiceWorkerClientSnapshot>)>>;
WEB_API void navigate_window_client(HTML::EnvironmentId const&, URL::URL const&, WindowClientNavigationCallback);

}
