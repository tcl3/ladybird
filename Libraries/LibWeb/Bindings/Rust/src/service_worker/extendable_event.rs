/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! ExtendableEvent, https://w3c.github.io/ServiceWorker/#extendableevent-interface

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{EmbedderCell, EmbedderClass, EmbedderValue, Gc, GcRefCell, Realm, Trace, Value, Vm};

use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::promise::{WebIdlPromise, queue_a_microtask, react_to_promise};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::dom::event::EventInit;

/// The dictionary ExtendableEventInit of ServiceWorker/ExtendableEvent.idl.
pub(crate) struct ExtendableEventInit {
    pub(crate) parent: EventInit,
}

/// What an ExtendableEvent extends its lifetime with. The reactions to the promises that waitUntil() adds hold it, as
/// they cannot hold the event, whose operations run on whichever object inherits from it.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ExtendableEventLifetime {
    event: CppEvent,

    // An ExtendableEvent object has an associated extend lifetime promises (an array of promises). It is initially an
    // empty array.
    extend_lifetime_promises: GcRefCell<Vec<WebIdlPromise>>,

    // An ExtendableEvent object has an associated pending promises count (the number of pending promises in the extend
    // lifetime promises). It is initially set to zero.
    pending_promises_count: Cell<u32>,

    // An ExtendableEvent object has an associated timed out flag. It is initially unset, and is set after an optional
    // user agent imposed delay if the pending promises count is greater than zero.
    timed_out_flag: Cell<bool>,

    // NB: The steps that run once the event is no longer active, which the algorithm that dispatched the event waits
    //     for.
    #[gc(untraced)]
    on_inactive: Cell<Option<InactiveSteps>>,
}

static EXTENDABLE_EVENT_LIFETIME_CLASS: EmbedderClass<ExtendableEventLifetime> =
    EmbedderClass::new("ExtendableEventLifetime");

impl EmbedderValue for ExtendableEventLifetime {
    const CLASS: &'static EmbedderClass<Self> = &EXTENDABLE_EVENT_LIFETIME_CLASS;
}

pub(crate) type ExtendableEventLifetimeRef = Gc<EmbedderCell<ExtendableEventLifetime>>;

/// The steps that run once an event is no longer active.
pub(crate) type InactiveSteps = fn(&Vm, ExtendableEventLifetimeRef);

impl ExtendableEventLifetime {
    // https://w3c.github.io/ServiceWorker/#extendableevent-active
    pub(crate) fn is_active(&self) -> bool {
        // An ExtendableEvent object is said to be active when its timed out flag is unset and either its pending promises
        // count is greater than zero or its dispatch flag is set.
        !self.timed_out_flag.get() && (self.pending_promises_count.get() > 0 || self.event.dispatched())
    }

    pub(crate) fn dispatch_flag(&self) -> bool {
        self.event.dispatched()
    }

    pub(crate) fn timed_out_flag(&self) -> bool {
        self.timed_out_flag.get()
    }

    pub(crate) fn extend_lifetime_promises(&self) -> Vec<WebIdlPromise> {
        self.extend_lifetime_promises.borrow().clone()
    }

    /// Runs the steps once the event is no longer active, right away if it is not.
    pub(crate) fn wait_until_not_active(this: ExtendableEventLifetimeRef, vm: &Vm, steps: InactiveSteps) {
        if !this.is_active() {
            steps(vm, this);
            return;
        }
        this.on_inactive.set(Some(steps));
    }

    /// Sets the timed out flag, which leaves the event no longer active.
    pub(crate) fn time_out(this: ExtendableEventLifetimeRef, vm: &Vm) {
        this.timed_out_flag.set(true);
        Self::run_steps_if_inactive(this, vm);
    }

    fn run_steps_if_inactive(this: ExtendableEventLifetimeRef, vm: &Vm) {
        if this.is_active() {
            return;
        }
        if let Some(steps) = this.on_inactive.take() {
            steps(vm, this);
        }
    }
}

// https://w3c.github.io/ServiceWorker/#extendableevent-interface
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ExtendableEvent {
    lifetime: ExtendableEventLifetimeRef,
}

impl Wrappable for ExtendableEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.lifetime.event.as_cpp_parent()
    }
}

impl ExtendableEvent {
    /// The ExtendableEvent of an event of an interface that inherits from ExtendableEvent, which owns the event.
    pub(crate) fn new(vm: &Vm, event: CppEvent) -> Self {
        let lifetime = EXTENDABLE_EVENT_LIFETIME_CLASS.allocate(vm, |_| ExtendableEventLifetime {
            event,
            extend_lifetime_promises: GcRefCell::new(Vec::new()),
            pending_promises_count: Cell::new(0),
            timed_out_flag: Cell::new(false),
            on_inactive: Cell::new(None),
        });
        Self { lifetime }
    }

    pub(crate) fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_type: &Utf16String,
        event_init: &ExtendableEventInit,
    ) -> Ref<Self> {
        let event = CppEvent::create_for_owner(
            realm,
            event_type,
            event_init.parent.bubbles,
            event_init.parent.cancelable,
            event_init.parent.composed,
        );
        let object = allocate(vm, |_| Self::new(vm, event));
        object.lifetime.event.set_owner(object);
        object
    }

    // https://w3c.github.io/ServiceWorker/#dom-extendableevent-extendableevent
    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: ExtendableEventInit,
    ) -> WebIdlResult<Ref<Self>> {
        Ok(Self::create(cx.vm(), cx.realm(), &r#type, &event_init_dict))
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.lifetime.event
    }

    pub(crate) fn lifetime(&self) -> ExtendableEventLifetimeRef {
        self.lifetime
    }

    // https://w3c.github.io/ServiceWorker/#dom-extendableevent-waituntil
    pub(crate) fn wait_until(&self, cx: &mut CallContext, f: WebIdlPromise) -> WebIdlResult<()> {
        // The waitUntil(f) method steps are to add lifetime promise f to this.
        add_lifetime_promise(cx.vm(), self.lifetime, f)
    }
}

// https://w3c.github.io/ServiceWorker/#extendableevent-add-lifetime-promise
pub(crate) fn add_lifetime_promise(
    vm: &Vm,
    event: ExtendableEventLifetimeRef,
    promise: WebIdlPromise,
) -> WebIdlResult<()> {
    // 1. If event's isTrusted attribute is false, throw an "InvalidStateError" DOMException.
    if !event.event.is_trusted() {
        return Err(Exception::Dom(
            "InvalidStateError",
            Utf16String::from_utf8("waitUntil() was called on an event that the user agent did not dispatch"),
        ));
    }

    // 2. If event is not active, throw an "InvalidStateError" DOMException.
    // NOTE: If no lifetime extension promise has been added in the task that called the event handlers, calling
    //       waitUntil() in subsequent asynchronous tasks will throw.
    if !event.is_active() {
        return Err(Exception::Dom(
            "InvalidStateError",
            Utf16String::from_utf8("waitUntil() was called on an event that is no longer active"),
        ));
    }

    // 3. Add promise to event's extend lifetime promises.
    event.extend_lifetime_promises.borrow_mut(vm).push(promise);

    // 4. Increment event's pending promises count by one.
    // NOTE: The pending promises count is incremented even if the given promise has already been settled. The
    //       corresponding count decrement is done in the microtask queued by the reaction to the promise.
    event.pending_promises_count.set(event.pending_promises_count.get() + 1);

    // 5. Upon fulfillment or rejection of promise, queue a microtask to run these substeps:
    fn on_settled(cx: &CallContext, event: ExtendableEventLifetimeRef, _: Value) -> WebIdlResult<Value> {
        queue_a_microtask(cx.vm(), cx.realm(), event, |vm, event| {
            // 1. Decrement event's pending promises count by one.
            event.pending_promises_count.set(event.pending_promises_count.get() - 1);

            // 2. If event's pending promises count is 0, then:
            if event.pending_promises_count.get() == 0 {
                // 1. Let registration be the current global object's associated service worker's containing service
                //    worker registration.
                // 2. If registration is unregistered, invoke Try Clear Registration with registration.
                // 3. If registration is not null, invoke Try Activate with registration.
                // NB: The worker's process tells the UI process once the worker has no pending events, which runs these
                //     steps then.
                ExtendableEventLifetime::run_steps_if_inactive(event, vm);
            }
        });
        Ok(Value::UNDEFINED)
    }
    react_to_promise(vm, promise, event, Some(on_settled), Some(on_settled));
    Ok(())
}
