/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! ServiceWorker, https://w3c.github.io/ServiceWorker/#serviceworker-interface

use core::cell::{Cell, RefCell};
use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{Gc, MarkedVec, Object, Realm, Trace, Value, Visitor, Vm};

use crate::bindings::conversions::UsvString;
use crate::bindings::event_target::{CppEvent, CppEventTarget, GetTheParent};
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::realm_data::realm_data;
use crate::bindings::{CallContext, WebIdlResult};
use crate::html::message_port::StructuredSerializeOptions;
use crate::html::structured_serialize::structured_serialize_with_transfer;
use crate::html::structured_serialize::transfer_record::TransferRecord;
use crate::service_worker::{LiveObjects, ServiceWorkerDescriptor};

unsafe extern "C" {
    /// Sends the message to the service worker, with the incumbent settings object as its source. LibWeb takes the
    /// record over.
    fn web_service_worker_post_message_to_service_worker(
        realm: *mut c_void,
        service_worker_id: u64,
        record: *mut c_void,
    );
}

// https://w3c.github.io/ServiceWorker/#enumdef-serviceworkerstate
/// In the order of Web::Bindings::ServiceWorkerState, which C++ passes as its underlying value.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum ServiceWorkerState {
    Parsed,
    Installing,
    Installed,
    Activating,
    Activated,
    Redundant,
}

// SAFETY: A state holds no cells.
unsafe impl Trace for ServiceWorkerState {
    fn trace(&self, _: &mut Visitor) {}
}

impl ServiceWorkerState {
    pub(crate) fn from_u8(value: u8) -> Option<Self> {
        match value {
            0 => Some(Self::Parsed),
            1 => Some(Self::Installing),
            2 => Some(Self::Installed),
            3 => Some(Self::Activating),
            4 => Some(Self::Activated),
            5 => Some(Self::Redundant),
            _ => None,
        }
    }
}

// https://w3c.github.io/ServiceWorker/#serviceworker-interface
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ServiceWorker {
    event_target: CppEventTarget,
    realm: Gc<Realm>,

    // Each ServiceWorker object is associated with a service worker.
    id: u64,
    #[gc(untraced)]
    script_url: UsvString,

    // A ServiceWorker object has an associated ServiceWorkerState object which is itself associated with service
    // worker's state.
    #[gc(untraced)]
    state: Cell<ServiceWorkerState>,
}

impl Wrappable for ServiceWorker {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.realm)
    }

    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event_target.as_cpp_parent()
    }
}

impl GetTheParent for ServiceWorker {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

thread_local! {
    static SERVICE_WORKER_OBJECTS: RefCell<LiveObjects<ServiceWorker>> = const { RefCell::new(LiveObjects::new()) };
}

/// Lets go of the objects of an agent that is going away, whose weak references would outlive its heap.
pub(crate) fn forget_service_worker_objects_of_agent() {
    SERVICE_WORKER_OBJECTS.with_borrow_mut(LiveObjects::clear);
}

impl ServiceWorker {
    pub(crate) fn id(&self) -> u64 {
        self.id
    }

    // https://w3c.github.io/ServiceWorker/#dom-serviceworker-scripturl
    pub(crate) fn script_url(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        // The scriptURL getter steps are to return the service worker's serialized script url.
        Ok(self.script_url.clone())
    }

    // https://w3c.github.io/ServiceWorker/#dom-serviceworker-state
    pub(crate) fn state(&self, _: &mut CallContext) -> WebIdlResult<ServiceWorkerState> {
        // The state attribute must return the value (in ServiceWorkerState enumeration) to which it was last set.
        Ok(self.state.get())
    }

    // https://w3c.github.io/ServiceWorker/#service-worker-postmessage
    pub(crate) fn post_message(
        &self,
        cx: &mut CallContext,
        message: Value,
        transfer: MarkedVec<'_, Gc<Object>>,
    ) -> WebIdlResult<()> {
        // 1. Let options be «[ "transfer" → transfer ]».
        // 2. Invoke postMessage(message, options) with message and options.
        self.post_message_steps(cx, message, &transfer.values())
    }

    // https://w3c.github.io/ServiceWorker/#service-worker-postmessage-options
    pub(crate) fn post_message_with_options(
        &self,
        cx: &mut CallContext,
        message: Value,
        options: StructuredSerializeOptions<'_>,
    ) -> WebIdlResult<()> {
        self.post_message_steps(cx, message, &options.transfer.values())
    }

    // https://w3c.github.io/ServiceWorker/#service-worker-postmessage-options
    fn post_message_steps(&self, cx: &CallContext, message: Value, transfer: &[Gc<Object>]) -> WebIdlResult<()> {
        // 1. Let serviceWorker be the service worker represented by this.
        // 2. Let incumbentSettings be the incumbent settings object.
        // 3. Let incumbentGlobal be incumbentSettings's global object.
        // NB: LibWeb reads the incumbent settings object when it sends the message.

        // 4. Let serializeWithTransferResult be StructuredSerializeWithTransfer(message, options["transfer"]). Rethrow
        //    any exceptions.
        let serialize_with_transfer_result = structured_serialize_with_transfer(cx, message, transfer)?;

        // 5. If the result of running the Should Skip Event algorithm with "message" and serviceWorker is true, then
        //    return.
        // 6. Run these substeps in parallel:
        // NB: The UI process runs step 5 and the substeps, and the service worker's process queues the task.
        // SAFETY: The realm is live, and LibWeb takes the record over.
        unsafe {
            web_service_worker_post_message_to_service_worker(
                self.realm.as_ptr().cast(),
                self.id,
                TransferRecord::into_cpp(serialize_with_transfer_result, cx.vm()).into_raw(),
            );
        }
        Ok(())
    }
}

// https://w3c.github.io/ServiceWorker/#get-the-service-worker-object
pub(crate) fn get_the_service_worker_object(
    vm: &Vm,
    realm: Gc<Realm>,
    service_worker: &ServiceWorkerDescriptor,
) -> Ref<ServiceWorker> {
    // 1. Let objectMap be environment's service worker object map.
    let data = realm_data(vm, realm);
    if let Some(service_worker_object) = data
        .service_worker_objects
        .borrow()
        .iter()
        .find(|object| object.id == service_worker.id)
    {
        // 3. Return objectMap[serviceWorker].
        return *service_worker_object;
    }

    // 2. If objectMap[serviceWorker] does not exist, then:
    // 1. Let serviceWorkerObj be a new ServiceWorker in environment's Realm, and associate it with serviceWorker.
    // 2. Set serviceWorkerObj's state to serviceWorker's state.
    let service_worker_object = allocate(vm, |_| ServiceWorker {
        event_target: CppEventTarget::new(),
        realm,
        id: service_worker.id,
        script_url: UsvString::from_utf8(&service_worker.script_url),
        state: Cell::new(service_worker.state),
    });
    service_worker_object.event_target.set_owner(service_worker_object);
    SERVICE_WORKER_OBJECTS.with_borrow_mut(|objects| objects.add(vm, service_worker_object));

    // 3. Set objectMap[serviceWorker] to serviceWorkerObj.
    data.service_worker_objects.borrow_mut(vm).push(service_worker_object);

    // 3. Return objectMap[serviceWorker].
    service_worker_object
}

// https://w3c.github.io/ServiceWorker/#update-state-algorithm
pub(crate) fn service_worker_state_changed(vm: &Vm, worker: &ServiceWorkerDescriptor) {
    // 4. For each settingsObject of settingsObjects, queue a task on settingsObject's responsible event loop in the DOM
    //    manipulation task source to run the following steps:
    // NB: The task that runs these steps is the one of the message from the UI process, which runs them for each
    //     realm of this process.
    let objects = SERVICE_WORKER_OBJECTS.with_borrow(|objects| objects.matching(vm, |object| object.id == worker.id));

    // 1. Let objectMap be settingsObject's service worker object map.
    // 2. If objectMap[worker] does not exist, then abort these steps.
    // 3. Let workerObj be objectMap[worker].
    for worker_object in objects.values().iter() {
        // 4. Set workerObj's state to state.
        worker_object.state.set(worker.state);

        // 5. Fire an event named statechange at workerObj.
        let event = CppEvent::create(worker_object.realm, "statechange", false, false, false);
        event.set_is_trusted(true);
        worker_object.event_target.dispatch_event(&event);
    }
}
