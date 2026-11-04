/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The message events that a service worker's process dispatches for ServiceWorker.postMessage(), and the service
//! worker's set of extended events, https://w3c.github.io/ServiceWorker/#dfn-set-of-extended-events

use core::cell::{Cell, RefCell};
use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{CellRoot, EmbedderCell, Gc, MarkedVec, Realm, Value, Vm, is_host_instance_of};

use crate::bindings::CallContext;
use crate::bindings::conversions::UsvString;
use crate::bindings::event_target::CppEventTarget;
use crate::bindings::interface::{Inherits, Interface, realm_arguments, with_temporary_execution_context};
use crate::dom::event::EventInit;
use crate::html::message_port::MessagePort;
use crate::html::structured_serialize::cpp::OwnedSerializedTransferRecord;
use crate::html::structured_serialize::structured_deserialize_with_transfer;
use crate::html::structured_serialize::transfer_record::TransferRecord;
use crate::service_worker::client::{AnyClient, Client, ServiceWorkerClientSnapshot};
use crate::service_worker::clients::ClientType;
use crate::service_worker::exports::{FfiServiceWorkerClientSnapshot, FfiServiceWorkerDescriptor};
use crate::service_worker::extendable_event::{
    ExtendableEventInit, ExtendableEventLifetime, ExtendableEventLifetimeRef,
};
use crate::service_worker::extendable_message_event::{
    ClientOrServiceWorkerOrMessagePort, ExtendableMessageEvent, ExtendableMessageEventFields,
};
use crate::service_worker::fetch_event::time_out_fetch_event;
use crate::service_worker::service_worker::get_the_service_worker_object;
use crate::service_worker::window_client::WindowClient;
use crate::service_worker::{ServiceWorkerDescriptor, exports};

/// Called with the context that the dispatch was given, whenever the set of extended events becomes empty or stops
/// being empty: with true when the worker has pending events, and false when it has none.
pub(crate) type OnPendingEventsChanged = unsafe extern "C" fn(*mut c_void, bool);

struct ExtendedEvent {
    id: u64,
    lifetime: ExtendableEventLifetimeRef,
    _root: CellRoot<EmbedderCell<ExtendableEventLifetime>>,
}

// https://w3c.github.io/ServiceWorker/#dfn-set-of-extended-events
struct SetOfExtendedEvents {
    // A service worker has an associated set of extended events (a set) whose item is an ExtendableEvent. It is
    // initially a new set.
    items: RefCell<Vec<ExtendedEvent>>,
    next_id: Cell<u64>,
    on_pending_events_changed: Cell<Option<(*mut c_void, OnPendingEventsChanged)>>,
}

thread_local! {
    static SET_OF_EXTENDED_EVENTS: SetOfExtendedEvents = const {
        SetOfExtendedEvents {
            items: RefCell::new(Vec::new()),
            next_id: Cell::new(0),
            on_pending_events_changed: Cell::new(None),
        }
    };
}

/// Lets go of the extended events of an agent that is going away, whose roots would outlive its heap. Its worker has run
/// its closing steps, so nothing waits to hear that the set became empty.
pub(crate) fn forget_extended_events_of_agent() {
    SET_OF_EXTENDED_EVENTS.with(|set| {
        set.on_pending_events_changed.set(None);
        set.items.borrow_mut().clear();
    });
}

/// Remembers what a dispatch was given to report whether the worker has pending events.
pub(crate) fn set_on_pending_events_changed(ctx: *mut c_void, on_pending_events_changed: OnPendingEventsChanged) {
    SET_OF_EXTENDED_EVENTS.with(|set| {
        set.on_pending_events_changed
            .set(Some((ctx, on_pending_events_changed)));
    });
}

fn report_pending_events(has_pending_events: bool) {
    let Some((ctx, on_pending_events_changed)) = SET_OF_EXTENDED_EVENTS.with(|set| set.on_pending_events_changed.get())
    else {
        return;
    };
    // SAFETY: The context and the function are what C++ passed with the event, which it keeps for as long as the
    //         process runs the worker.
    unsafe { on_pending_events_changed(ctx, has_pending_events) };
}

fn extended_event_is_not_active(_: &Vm, lifetime: ExtendableEventLifetimeRef) {
    let became_empty = SET_OF_EXTENDED_EVENTS.with(|set| {
        let mut items = set.items.borrow_mut();
        let had_items = !items.is_empty();
        items.retain(|item| item.lifetime != lifetime);
        had_items && items.is_empty()
    });
    if became_empty {
        report_pending_events(false);
    }
}

// https://w3c.github.io/ServiceWorker/#update-service-worker-extended-events-set-algorithm
/// Returns the id of the event if the event is in the set afterwards, which the event's deadline names it by.
pub(crate) fn update_service_worker_extended_events_set(vm: &Vm, lifetime: ExtendableEventLifetimeRef) -> Option<u64> {
    // 1. Assert: event's dispatch flag is unset.
    assert!(!lifetime.dispatch_flag());

    // 2. For each item of worker's set of extended events:
    //    1. If item is not active, remove item from worker's set of extended events.
    // NB: An item is removed as soon as it is no longer active.

    // 3. If event is active, append event to worker's set of extended events.
    if !lifetime.is_active() {
        return None;
    }
    let (id, became_non_empty) = SET_OF_EXTENDED_EVENTS.with(|set| {
        let id = set.next_id.get() + 1;
        set.next_id.set(id);
        let mut items = set.items.borrow_mut();
        let became_non_empty = items.is_empty();
        items.push(ExtendedEvent {
            id,
            lifetime,
            _root: CellRoot::new(lifetime),
        });
        (id, became_non_empty)
    });
    if became_non_empty {
        report_pending_events(true);
    }
    ExtendableEventLifetime::wait_until_not_active(lifetime, vm, extended_event_is_not_active);
    Some(id)
}

/// The source of a message that the UI process says the service worker received.
enum MessageSource {
    Client(ServiceWorkerClientSnapshot),
    ServiceWorker(ServiceWorkerDescriptor),
}

// https://w3c.github.io/ServiceWorker/#service-worker-postmessage-options
fn dispatch_message_event(
    vm: &Vm,
    realm: Gc<Realm>,
    destination: CppEventTarget,
    message_source: MessageSource,
    origin: UsvString,
    serialize_with_transfer_result: OwnedSerializedTransferRecord,
) -> Option<u64> {
    // 6.2.1. Let source be determined by switching on the type of incumbentGlobal:
    //        ServiceWorkerGlobalScope
    //            The result of getting the service worker object that represents incumbentGlobal's service worker in
    //            the relevant settings object of serviceWorker's global object.
    //        Window
    //            a new WindowClient object that represents incumbentGlobal's relevant settings object.
    //        Otherwise
    //            a new Client object that represents incumbentGlobal's associated worker
    let source = match &message_source {
        MessageSource::ServiceWorker(service_worker) => {
            ClientOrServiceWorkerOrMessagePort::ServiceWorker(get_the_service_worker_object(vm, realm, service_worker))
        }
        MessageSource::Client(client) if client.r#type == ClientType::Window => {
            ClientOrServiceWorkerOrMessagePort::Client(AnyClient::WindowClient(WindowClient::create(vm, realm, client)))
        }
        MessageSource::Client(client) => {
            ClientOrServiceWorkerOrMessagePort::Client(AnyClient::Client(Client::create(vm, realm, client)))
        }
    };

    // 6.2.2. Let origin be incumbentSettings's origin.
    // 6.2.3. Let destination be the ServiceWorkerGlobalScope object associated with serviceWorker.
    let event_init = ExtendableEventInit {
        parent: EventInit {
            bubbles: false,
            cancelable: false,
            composed: false,
        },
    };

    // 6.2.4. Let deserializeRecord be StructuredDeserializeWithTransfer(serializeWithTransferResult, destination's
    //        Realm). If this throws an exception, let e be the result of creating an event named messageerror, using
    //        ExtendableMessageEvent, with its origin initialized to origin and the source attribute initialized to
    //        source.
    let cx = CallContext::new(vm, realm);
    // NB: The execution context must not stay on the stack while the event is dispatched, so that a microtask
    //     checkpoint can be performed after each listener.
    let serialize_with_transfer_result = TransferRecord::from_cpp(vm, serialize_with_transfer_result.record());
    let deserialize_record = with_temporary_execution_context(realm, || {
        structured_deserialize_with_transfer(&cx, serialize_with_transfer_result)
    });
    TransferRecord::discard(serialize_with_transfer_result);
    let event = match deserialize_record {
        Err(_) => ExtendableMessageEvent::create(
            vm,
            realm,
            &Utf16String::from_ascii(b"messageerror"),
            &event_init,
            ExtendableMessageEventFields {
                data: Value::NULL,
                origin,
                last_event_id: Utf16String::from_ascii(b""),
                source: Some(source),
            },
            &MarkedVec::new(vm),
        ),
        // 6.2.5. Else:
        Ok(deserialize_record) => {
            // 1. Let messageClone be deserializeRecord.[[Deserialized]].
            // 2. Let newPorts be a new frozen array consisting of all MessagePort objects in
            //    deserializeRecord.[[TransferredValues]], if any, maintaining their relative order.
            let new_ports = MarkedVec::new(vm);
            for value in deserialize_record.transferred_values.values().iter() {
                if is_host_instance_of(value, MessagePort::wrapper_class()) {
                    new_ports.push(Value::from_object(*value));
                }
            }

            // 3. Let e be the result of creating an event named message, using ExtendableMessageEvent, with its origin
            //    initialized to origin, the source attribute initialized to source, the data attribute initialized to
            //    messageClone, and the ports attribute initialized to newPorts.
            ExtendableMessageEvent::create(
                vm,
                realm,
                &Utf16String::from_ascii(b"message"),
                &event_init,
                ExtendableMessageEventFields {
                    data: deserialize_record.deserialized,
                    origin,
                    last_event_id: Utf16String::from_ascii(b""),
                    source: Some(source),
                },
                &new_ports,
            )
        }
    };
    let cpp_event = event.parent().event().clone();
    cpp_event.set_is_trusted(true);

    // 6.2.6. Dispatch e at destination.
    destination.dispatch_event(&cpp_event);

    // 6.2.7. Invoke Update Service Worker Extended Events Set with serviceWorker and e.
    update_service_worker_extended_events_set(vm, event.parent().lifetime())
}

/// Dispatches the message event of a message that the UI process says the service worker received, at the global
/// object, whose EventTarget `global` is. The source is the client, or else the service worker. Returns the id that
/// the event is in the set of extended events with, or 0 if the event is not in it, and calls
/// `on_pending_events_changed` with `ctx` whenever the set becomes empty or stops being empty.
///
/// # Safety
///
/// The pointers must be the live VM, a live realm and the realm's live global EventTarget. One of `client` and
/// `worker` must be lent for the call, and the other null. LibWeb hands `record` over. `on_pending_events_changed`
/// must be callable with `ctx` for as long as the process runs the worker.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn web_rust_service_worker_dispatch_message_event(
    vm: *mut c_void,
    realm: *mut c_void,
    global: *mut c_void,
    client: *const FfiServiceWorkerClientSnapshot<'_>,
    worker: *const FfiServiceWorkerDescriptor<'_>,
    origin: ak::Span<'_, u8>,
    record: *mut c_void,
    ctx: *mut c_void,
    on_pending_events_changed: OnPendingEventsChanged,
) -> u64 {
    // SAFETY: The caller passes the live VM, a live realm, its global EventTarget, a lent source and the record.
    let (vm, realm, global, source, record) = unsafe {
        let (vm, realm) = realm_arguments(vm, realm);
        let source = if client.is_null() {
            MessageSource::ServiceWorker(exports::descriptor_from_abi(&*worker).expect("C++ passes a service worker"))
        } else {
            MessageSource::Client(exports::client_snapshot_from_abi(&*client))
        };
        (
            vm,
            realm,
            CppEventTarget::from_raw(global),
            source,
            OwnedSerializedTransferRecord::from_raw(record),
        )
    };
    set_on_pending_events_changed(ctx, on_pending_events_changed);
    let origin = UsvString::from_utf8(&String::from_utf8_lossy(origin.as_slice()));
    dispatch_message_event(vm, realm, global, source, origin, record).unwrap_or(0)
}

/// Sets the timed out flag of the event in the set of extended events with the id, if it is still there.
///
/// # Safety
///
/// `vm` must be the live VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_service_worker_time_out_extended_event(vm: *mut c_void, id: u64) {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { &*vm.cast::<Vm>() };
    let lifetime = SET_OF_EXTENDED_EVENTS.with(|set| {
        set.items
            .borrow()
            .iter()
            .find(|item| item.id == id)
            .map(|item| item.lifetime)
    });
    if let Some(lifetime) = lifetime {
        ExtendableEventLifetime::time_out(lifetime, vm);
    }
    time_out_fetch_event(vm, id);
}
