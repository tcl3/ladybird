/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The install and activate events, which the service worker's process dispatches for the Install and Activate
//! algorithms of the UI process.

use core::cell::RefCell;
use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{CellRoot, EmbedderCell, Gc, Realm, Value, Vm};

use crate::bindings::CallContext;
use crate::bindings::event_target::CppEventTarget;
use crate::bindings::interface::{Inherits, realm_arguments, with_temporary_execution_context_and_callbacks};
use crate::bindings::promise::{get_promise_for_wait_for_all, promise_realm, react_to_promise};
use crate::dom::event::EventInit;
use crate::service_worker::extendable_event::{
    ExtendableEvent, ExtendableEventInit, ExtendableEventLifetime, ExtendableEventLifetimeRef,
};
use crate::service_worker::install_event::InstallEvent;

/// In the order of Web::ServiceWorker::ServiceWorkerLifecycleEvent, which C++ passes as its underlying value.
#[derive(Clone, Copy, PartialEq, Eq)]
enum LifecycleEvent {
    Install,
    Activate,
}

/// Called once the event that web_rust_service_worker_dispatch_lifecycle_event() dispatched is no longer active, with
/// the context it was given, and, for an install event, whether installation failed.
type OnLifecycleEventFinished = unsafe extern "C" fn(*mut c_void, bool);

struct LifecycleEventInFlight {
    kind: LifecycleEvent,
    lifetime: ExtendableEventLifetimeRef,
    _root: CellRoot<EmbedderCell<ExtendableEventLifetime>>,
    ctx: *mut c_void,
    on_finished: OnLifecycleEventFinished,
}

thread_local! {
    static LIFECYCLE_EVENT_IN_FLIGHT: RefCell<Option<LifecycleEventInFlight>> = const { RefCell::new(None) };
}

/// Lets go of the event of an agent that is going away, whose root would outlive its heap. Its worker has run its
/// closing steps, so nothing waits for the event to finish.
pub(crate) fn forget_lifecycle_event_of_agent() {
    drop(LIFECYCLE_EVENT_IN_FLIGHT.with_borrow_mut(Option::take));
}

fn finish(failed: bool) {
    let Some(in_flight) = LIFECYCLE_EVENT_IN_FLIGHT.with_borrow_mut(Option::take) else {
        return;
    };
    // SAFETY: The context and the function are what C++ passed with the event, which it keeps for as long as the
    //         process runs the worker.
    unsafe { (in_flight.on_finished)(in_flight.ctx, failed) };
}

fn kind_of(lifetime: ExtendableEventLifetimeRef) -> Option<LifecycleEvent> {
    LIFECYCLE_EVENT_IN_FLIGHT.with_borrow(|in_flight| {
        in_flight
            .as_ref()
            .filter(|in_flight| in_flight.lifetime == lifetime)
            .map(|in_flight| in_flight.kind)
    })
}

fn event_is_not_active(vm: &Vm, lifetime: ExtendableEventLifetimeRef) {
    match kind_of(lifetime) {
        Some(LifecycleEvent::Install) => {
            // https://w3c.github.io/ServiceWorker/#installation-algorithm
            // 11.3.1.4.2. If e's timed out flag is set, set installFailed to true.
            if lifetime.timed_out_flag() {
                finish(true);
                return;
            }

            // 11.3.1.4.3. Let p be the result of getting a promise to wait for all of e's extend lifetime promises.
            let promises = lifetime.extend_lifetime_promises();
            let Some(first_promise) = promises.first() else {
                finish(false);
                return;
            };
            let realm = promise_realm(*first_promise);
            with_temporary_execution_context_and_callbacks(realm, || {
                let p = get_promise_for_wait_for_all(vm, realm, &promises);

                // 11.3.1.4.4. Upon rejection of p, set installFailed to true.
                react_to_promise(
                    vm,
                    p,
                    (),
                    Some(|_: &CallContext, (), _: Value| {
                        finish(false);
                        Ok(Value::UNDEFINED)
                    }),
                    Some(|_: &CallContext, (), _: Value| {
                        finish(true);
                        Ok(Value::UNDEFINED)
                    }),
                );
            });
        }
        // https://w3c.github.io/ServiceWorker/#activation-algorithm
        // 10.1.1.4. WaitForAsynchronousExtensions: Wait, in parallel, until e is not active.
        Some(LifecycleEvent::Activate) => finish(false),
        None => {}
    }
}

fn dispatch(
    vm: &Vm,
    realm: Gc<Realm>,
    global: CppEventTarget,
    kind: LifecycleEvent,
    ctx: *mut c_void,
    on_finished: OnLifecycleEventFinished,
) {
    let event_init = ExtendableEventInit {
        parent: EventInit {
            bubbles: false,
            cancelable: false,
            composed: false,
        },
    };

    let (event, lifetime) = match kind {
        // https://w3c.github.io/ServiceWorker/#installation-algorithm
        // 11.3.1.1. Let e be the result of creating an event with InstallEvent.
        // 11.3.1.2. Initialize e's type attribute to install.
        LifecycleEvent::Install => {
            let event = InstallEvent::create(vm, realm, &Utf16String::from_ascii(b"install"), &event_init);
            (event.parent().event().clone(), event.parent().lifetime())
        }
        // https://w3c.github.io/ServiceWorker/#activation-algorithm
        // 10.1.1.1. Let e be the result of creating an event with ExtendableEvent.
        // 10.1.1.2. Initialize e's type attribute to activate.
        LifecycleEvent::Activate => {
            let event = ExtendableEvent::create(vm, realm, &Utf16String::from_ascii(b"activate"), &event_init);
            (event.event().clone(), event.lifetime())
        }
    };
    event.set_is_trusted(true);

    LIFECYCLE_EVENT_IN_FLIGHT.with_borrow_mut(|in_flight| {
        *in_flight = Some(LifecycleEventInFlight {
            kind,
            lifetime,
            _root: CellRoot::new(lifetime),
            ctx,
            on_finished,
        });
    });

    // 11.3.1.3. Dispatch e at installingWorker's global object.
    // 10.1.1.3. Dispatch e at activeWorker's global object.
    global.dispatch_event(&event);

    // 11.3.1.4. WaitForAsynchronousExtensions: Run the following substeps in parallel:
    //     1. Wait until e is not active.
    ExtendableEventLifetime::wait_until_not_active(lifetime, vm, event_is_not_active);
}

/// Dispatches the lifecycle event that `kind` names, a Web::ServiceWorker::ServiceWorkerLifecycleEvent, at the global
/// object, whose EventTarget `global` is, and calls `on_finished` with `ctx` once the event is no longer active.
///
/// # Safety
///
/// The pointers must be the live VM, a live realm and the realm's live global EventTarget. `on_finished` must be
/// callable with `ctx` for as long as the process runs the worker.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_service_worker_dispatch_lifecycle_event(
    vm: *mut c_void,
    realm: *mut c_void,
    global: *mut c_void,
    kind: u8,
    ctx: *mut c_void,
    on_finished: OnLifecycleEventFinished,
) {
    // SAFETY: The caller passes the live VM, a live realm and its global EventTarget.
    let (vm, realm, global) = unsafe {
        let (vm, realm) = realm_arguments(vm, realm);
        (vm, realm, CppEventTarget::from_raw(global))
    };
    let kind = match kind {
        0 => LifecycleEvent::Install,
        1 => LifecycleEvent::Activate,
        _ => unreachable!("C++ passes a ServiceWorkerLifecycleEvent"),
    };
    dispatch(vm, realm, global, kind, ctx, on_finished);
}

/// Sets the timed out flag of the lifecycle event in flight, if there is one.
///
/// # Safety
///
/// `vm` must be the live VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_service_worker_time_out_lifecycle_event(vm: *mut c_void) {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { &*vm.cast::<Vm>() };
    let lifetime =
        LIFECYCLE_EVENT_IN_FLIGHT.with_borrow(|in_flight| in_flight.as_ref().map(|in_flight| in_flight.lifetime));
    if let Some(lifetime) = lifetime {
        ExtendableEventLifetime::time_out(lifetime, vm);
    }
}
