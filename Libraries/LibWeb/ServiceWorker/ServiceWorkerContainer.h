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
WEB_API void enable_client_message_queue(HTML::EnvironmentSettingsObject&);
WEB_API GC::Ref<ServiceWorker> service_worker_object(JS::Realm&, ServiceWorkerDescriptor const&);
WEB_API GC::Ref<ServiceWorkerRegistration> service_worker_registration_object(JS::Realm&, ServiceWorkerRegistrationSnapshot const&);

// Dispatches the message event of a message that the service worker received, and returns the id of the event in the
// worker's set of extended events, or 0 if the event is not in the set. The callback is called with the context
// whenever the set becomes empty or stops being empty.
WEB_API u64 dispatch_message_event(ServiceWorkerGlobalScope&, ServiceWorkerMessageSource const&, URL::Origin const&, HTML::SerializedTransferRecord, void* ctx, void (*on_pending_events_changed)(void*, bool has_pending_events));
WEB_API void time_out_extended_event(u64 event_id);

WEB_API ServiceWorkerId service_worker_id(ServiceWorker const&);

}
