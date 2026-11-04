/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! FetchEvent, https://w3c.github.io/ServiceWorker/#fetchevent-interface

use core::cell::{Cell, RefCell};
use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{CellRoot, EmbedderCell, Gc, GcRefCell, Object, Realm, Trace, Value, Vm};

use crate::bindings::conversions::CppInterface;
use crate::bindings::event_target::{CppEvent, CppEventTarget};
use crate::bindings::interface::{
    Inherits, Ref, Wrappable, Wrapped, allocate, realm_arguments, with_temporary_execution_context,
    with_temporary_execution_context_and_callbacks,
};
use crate::bindings::promise::{
    WebIdlPromise, create_promise, create_resolved_promise, promise_realm, react_to_promise,
    reject_promise_with_exception, resolve_promise,
};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::dom::event::EventInit;
use crate::service_worker::extendable_event::{ExtendableEvent, ExtendableEventInit, add_lifetime_promise};
use crate::service_worker::messages::{
    OnPendingEventsChanged, set_on_pending_events_changed, update_service_worker_extended_events_set,
};

unsafe extern "C" {
    /// Whether the object is a wrapper of a Response object.
    fn web_service_worker_is_response_object(object: *mut c_void) -> bool;
}

/// How Create Fetch Event and Dispatch ends, in the order of Web::ServiceWorker::FetchEventOutcome, which C++ takes as
/// its underlying value.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
enum FetchEventOutcome {
    Fallback,
    NetworkError,
    Response,
}

/// Called with the context that the dispatch was given once Create Fetch Event and Dispatch knows what it returns, with
/// the wrapper of the response object for a response, and null otherwise.
type OnFetchEventSettled = unsafe extern "C" fn(*mut c_void, u8, *mut c_void);

/// What the steps of Create Fetch Event and Dispatch that wait for the event remember of it.
#[derive(Clone, Copy)]
struct DispatchedFetchEvent {
    respond_with_entered: bool,
    event_canceled: bool,
    ctx: *mut c_void,
    on_settled: OnFetchEventSettled,
}

/// The dictionary FetchEventInit of ServiceWorker/FetchEvent.idl, whose members the bindings convert.
pub(crate) struct FetchEventInit {
    pub(crate) parent: ExtendableEventInit,
    pub(crate) request: CppInterface,
    pub(crate) preload_response: Option<WebIdlPromise>,
    pub(crate) client_id: Utf16String,
    pub(crate) resulting_client_id: Utf16String,
    pub(crate) replaces_client_id: Utf16String,
    pub(crate) handled: Option<WebIdlPromise>,
}

// https://w3c.github.io/ServiceWorker/#fetchevent-interface
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct FetchEvent {
    extendable_event: ExtendableEvent,
    request: CppInterface,
    preload_response: WebIdlPromise,
    #[gc(untraced)]
    client_id: Utf16String,
    #[gc(untraced)]
    resulting_client_id: Utf16String,
    #[gc(untraced)]
    replaces_client_id: Utf16String,
    handled: WebIdlPromise,

    // Each event using FetchEvent interface has an associated potential response (a response), initially set to null,
    // and the following associated flags that are initially unset:
    // - wait to respond flag
    // - respond-with entered flag
    // - respond-with error flag
    // NB: The potential response is the Response object that respondWith() was fulfilled with. The worker's process
    //     reads its body.
    potential_response: GcRefCell<Option<Gc<Object>>>,
    #[gc(untraced)]
    wait_to_respond_flag: Cell<bool>,
    #[gc(untraced)]
    respond_with_entered_flag: Cell<bool>,
    #[gc(untraced)]
    respond_with_error_flag: Cell<bool>,

    // NB: The steps of Create Fetch Event and Dispatch that wait until the wait to respond flag is unset, once the
    //     event's dispatch has returned while it was set.
    #[gc(untraced)]
    waiting_dispatch: Cell<Option<DispatchedFetchEvent>>,
    // NB: The id of the event in the set of extended events while its dispatch waits, which its deadline names it by.
    #[gc(untraced)]
    waiting_extended_event_id: Cell<Option<u64>>,
}

thread_local! {
    // NB: The fetch events whose dispatch waits until their wait to respond flag is unset, by the id of each in the
    //     set of extended events. Each stays alive until its flag is unset or its deadline passes.
    static FETCH_EVENTS_WAITING_TO_RESPOND: RefCell<Vec<WaitingFetchEvent>> = const { RefCell::new(Vec::new()) };
}

/// Lets go of the fetch events of an agent that is going away, whose roots would outlive its heap.
pub(crate) fn forget_fetch_events_of_agent() {
    drop(FETCH_EVENTS_WAITING_TO_RESPOND.with_borrow_mut(core::mem::take));
}

/// A fetch event whose dispatch waits, the id of its deadline, and the root that keeps it alive meanwhile.
type WaitingFetchEvent = (u64, Ref<FetchEvent>, CellRoot<EmbedderCell<Wrapped<FetchEvent>>>);

impl Wrappable for FetchEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.extendable_event.cpp_parent()
    }
}

impl Inherits<ExtendableEvent> for FetchEvent {
    fn parent(&self) -> &ExtendableEvent {
        &self.extendable_event
    }
}

impl FetchEvent {
    fn create(vm: &Vm, realm: Gc<Realm>, event_type: &Utf16String, event_init: FetchEventInit) -> Ref<Self> {
        let event = CppEvent::create_for_owner(
            realm,
            event_type,
            event_init.parent.parent.bubbles,
            event_init.parent.parent.cancelable,
            event_init.parent.parent.composed,
        );

        // When an event is created the attribute must be initialized to a promise resolved with undefined.
        let preload_response = event_init
            .preload_response
            .unwrap_or_else(|| create_resolved_promise(vm, realm, Value::UNDEFINED));

        // When an event is created the attribute must be initialized to a pending promise.
        let handled = event_init.handled.unwrap_or_else(|| create_promise(vm, realm));

        let object = allocate(vm, |_| Self {
            extendable_event: ExtendableEvent::new(vm, event),
            request: event_init.request,
            preload_response,
            client_id: event_init.client_id,
            resulting_client_id: event_init.resulting_client_id,
            replaces_client_id: event_init.replaces_client_id,
            handled,
            potential_response: GcRefCell::new(None),
            wait_to_respond_flag: Cell::new(false),
            respond_with_entered_flag: Cell::new(false),
            respond_with_error_flag: Cell::new(false),
            waiting_dispatch: Cell::new(None),
            waiting_extended_event_id: Cell::new(None),
        });
        object.extendable_event.event().set_owner(object);
        object
    }

    // https://w3c.github.io/ServiceWorker/#dom-fetchevent-fetchevent
    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: FetchEventInit,
    ) -> WebIdlResult<Ref<Self>> {
        Ok(Self::create(cx.vm(), cx.realm(), &r#type, event_init_dict))
    }

    // https://w3c.github.io/ServiceWorker/#fetch-event-request
    pub(crate) fn request(&self, _: &mut CallContext) -> WebIdlResult<CppInterface> {
        // request attribute must return the value it was initialized to.
        Ok(self.request)
    }

    // https://w3c.github.io/ServiceWorker/#fetch-event-preloadresponse
    pub(crate) fn preload_response(&self, _: &mut CallContext) -> WebIdlResult<WebIdlPromise> {
        // preloadResponse attribute must return the value it was initialized to.
        Ok(self.preload_response)
    }

    // https://w3c.github.io/ServiceWorker/#fetch-event-clientid
    pub(crate) fn client_id(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // clientId attribute must return the value it was initialized to.
        Ok(self.client_id.clone())
    }

    // https://w3c.github.io/ServiceWorker/#fetch-event-resultingclientid
    pub(crate) fn resulting_client_id(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // resultingClientId attribute must return the value it was initialized to.
        Ok(self.resulting_client_id.clone())
    }

    // https://w3c.github.io/ServiceWorker/#fetch-event-replacesClientId
    pub(crate) fn replaces_client_id(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // replacesClientId attribute must return the value it was initialized to.
        Ok(self.replaces_client_id.clone())
    }

    // https://w3c.github.io/ServiceWorker/#fetch-event-handled
    pub(crate) fn handled(&self, _: &mut CallContext) -> WebIdlResult<WebIdlPromise> {
        // handled attribute must return the value it was initialized to.
        Ok(self.handled)
    }

    // https://w3c.github.io/ServiceWorker/#fetch-event-respondwith
    pub(crate) fn respond_with(this: &Ref<Self>, cx: &mut CallContext, r: WebIdlPromise) -> WebIdlResult<()> {
        let vm = cx.vm();

        // 1. Let event be this.
        let event = this.extendable_event.event();

        // 2. If event's dispatch flag is unset, throw an "InvalidStateError" DOMException.
        if !event.dispatched() {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_utf8("respondWith() was called on an event that is not being dispatched"),
            ));
        }

        // 3. If event's respond-with entered flag is set, throw an "InvalidStateError" DOMException.
        if this.respond_with_entered_flag.get() {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_utf8("respondWith() was already called on the event"),
            ));
        }

        // 4. Add lifetime promise r to event.
        // NOTE: event.respondWith(r) extends the lifetime of the event by default as if event.waitUntil(r) is called.
        add_lifetime_promise(vm, this.extendable_event.lifetime(), r)?;

        // 5. Set event's stop propagation flag and stop immediate propagation flag.
        event.stop_immediate_propagation();

        // 6. Set event's respond-with entered flag.
        this.respond_with_entered_flag.set(true);

        // 7. Set event's wait to respond flag.
        this.wait_to_respond_flag.set(true);

        // 8. Let targetRealm be event's relevant Realm.
        // NB: The worker's process reads the response's body in it.

        // 9. Upon rejection of r:
        fn on_rejected(cx: &CallContext, event: Ref<FetchEvent>, _: Value) -> WebIdlResult<Value> {
            // 1. Set event's respond-with error flag.
            event.respond_with_error_flag.set(true);

            // 2. Unset event's wait to respond flag.
            event.unset_wait_to_respond_flag(cx.vm());
            Ok(Value::UNDEFINED)
        }

        // 10. Upon fulfillment of r with response:
        fn on_fulfilled(cx: &CallContext, event: Ref<FetchEvent>, response: Value) -> WebIdlResult<Value> {
            // 1. If response is not a Response object, then set the respond-with error flag.
            // NOTE: If the respond-with error flag is set, a network error is returned to Fetch through Handle Fetch
            //       algorithm. (See the step 21.1.) Otherwise, the value response is returned to Fetch through Handle
            //       Fetch algorithm. (See the step 22.1.)
            // SAFETY: The object is live.
            if !response.is_object()
                || !unsafe { web_service_worker_is_response_object(response.as_object().as_ptr().cast()) }
            {
                event.respond_with_error_flag.set(true);
            }
            // 2. Else:
            //    1. Let bytes be an empty byte sequence.
            //    [...]
            //    6. Set event's potential response to potentialResponse.
            // NB: The worker's process reads response's body, as these substeps do, once Handle Fetch returns the
            //     response.
            else {
                *event.potential_response.borrow_mut(cx.vm()) = Some(response.as_object());
            }

            // 3. Unset event's wait to respond flag.
            event.unset_wait_to_respond_flag(cx.vm());
            Ok(Value::UNDEFINED)
        }

        react_to_promise(vm, r, *this, Some(on_fulfilled), Some(on_rejected));
        Ok(())
    }

    fn unset_wait_to_respond_flag(&self, vm: &Vm) {
        self.wait_to_respond_flag.set(false);
        if let Some(id) = self.waiting_extended_event_id.take() {
            FETCH_EVENTS_WAITING_TO_RESPOND.with_borrow_mut(|events| events.retain(|(event_id, _, _)| *event_id != id));
        }
        if let Some(dispatched) = self.waiting_dispatch.take() {
            self.finish_create_fetch_event_and_dispatch(vm, dispatched, false);
        }
    }

    // https://w3c.github.io/ServiceWorker/#create-fetch-event-and-dispatch
    // NB: The steps that follow the event's task, which run once the wait to respond flag is unset.
    fn finish_create_fetch_event_and_dispatch(&self, vm: &Vm, dispatched: DispatchedFetchEvent, task_discarded: bool) {
        // 17.4.16. If e's wait to respond flag is set, then:
        //          1. Wait until e's wait to respond flag is unset.
        //          2. If e's respond-with error flag is set, set handleFetchFailed to true.
        //          3. Else, set response to e's potential response.
        // 17.4. If task is discarded, set handleFetchFailed to true.
        let handle_fetch_failed =
            task_discarded || (dispatched.respond_with_entered && self.respond_with_error_flag.get());
        let response = if dispatched.respond_with_entered && !handle_fetch_failed {
            *self.potential_response.borrow()
        } else {
            None
        };

        // 17.4.17. If response is null, request's body is not null, and request's body's source is null, then:
        // NB: The body of a request that crosses into the worker's process always has a source.
        // FIXME: 17.4.18. If response is not null, then set response's service worker timing info to timingInfo.

        let settle = |outcome: FetchEventOutcome, response: Option<Gc<Object>>| {
            let response = response.map_or(core::ptr::null_mut(), |response| response.as_ptr().cast());
            // SAFETY: The context and the function are what C++ passed with the event, which it keeps until it is
            //         called.
            unsafe { (dispatched.on_settled)(dispatched.ctx, outcome as u8, response) };
        };
        let network_error = || {
            Exception::Dom(
                "NetworkError",
                Utf16String::from_utf8("The service worker did not respond to the fetch event with a response"),
            )
        };

        // 21. If respondWithEntered is false, then:
        if !dispatched.respond_with_entered {
            // 1. If eventCanceled is true, then:
            if dispatched.event_canceled {
                // 1. If eventHandled is not null, then reject eventHandled with a "NetworkError" DOMException in
                //    workerRealm.
                reject_promise_with_exception(vm, self.handled, network_error());

                // 2. Return networkError.
                settle(FetchEventOutcome::NetworkError, None);
                return;
            }

            // 2. If eventHandled is not null, then resolve eventHandled.
            resolve_promise(vm, self.handled, Value::UNDEFINED);

            // FIXME: 3. If raceResponse is not null, and raceResponse's value is not null, then:

            // 4. Return null.
            settle(FetchEventOutcome::Fallback, None);
            return;
        }

        // 22. If handleFetchFailed is true, then:
        if handle_fetch_failed {
            // 1. If eventHandled is not null, then reject eventHandled with a "NetworkError" DOMException in
            //    workerRealm.
            reject_promise_with_exception(vm, self.handled, network_error());

            // 2. Return networkError.
            settle(FetchEventOutcome::NetworkError, None);
            return;
        }

        // 23. If eventHandled is not null, then resolve eventHandled.
        resolve_promise(vm, self.handled, Value::UNDEFINED);

        // 24. Return response.
        settle(FetchEventOutcome::Response, response);
    }
}

/// The ids by which a fetch event names its clients.
pub(crate) struct FetchEventClientIds {
    pub(crate) client_id: Utf16String,
    pub(crate) resulting_client_id: Utf16String,
    pub(crate) replaces_client_id: Utf16String,
}

// https://w3c.github.io/ServiceWorker/#create-fetch-event-and-dispatch
fn dispatch_fetch_event(
    vm: &Vm,
    realm: Gc<Realm>,
    global: CppEventTarget,
    request: CppInterface,
    client_ids: FetchEventClientIds,
    ctx: *mut c_void,
    on_settled: OnFetchEventSettled,
) -> Option<u64> {
    let event = with_temporary_execution_context(realm, || {
        // 17.2. Set eventHandled to a new promise in workerRealm.
        let event_handled = create_promise(vm, realm);

        // 17.4.1. Let e be the result of creating an event with FetchEvent.
        // 17.4.4. Initialize e's type attribute to fetch.
        // 17.4.5. Initialize e's cancelable attribute to true.
        // 17.4.6. Initialize e's request attribute to requestObject.
        // FIXME: 17.4.7. Initialize e's preloadResponse to preloadResponse.
        // 17.4.8. If client is not null, initialize e's clientId attribute to client's id.
        // 17.4.9. If request is a non-subresource request, request's destination is not "report", and reservedClient
        //         is not null, initialize e's resultingClientId attribute to reservedClient's id.
        // 17.4.10. If request is a navigation request, initialize e's replacesClientId attribute to request's replaces
        //          client id.
        // 17.4.11. Initialize e's handled to eventHandled.
        // NB: The worker's process created requestObject, and names the clients.
        let event_init = FetchEventInit {
            parent: ExtendableEventInit {
                parent: EventInit {
                    bubbles: false,
                    cancelable: true,
                    composed: false,
                },
            },
            request,
            preload_response: None,
            client_id: client_ids.client_id,
            resulting_client_id: client_ids.resulting_client_id,
            replaces_client_id: client_ids.replaces_client_id,
            handled: Some(event_handled),
        };
        FetchEvent::create(vm, realm, &Utf16String::from_ascii(b"fetch"), event_init)
    });
    let cpp_event = event.extendable_event.event().clone();
    cpp_event.set_is_trusted(true);

    // FIXME: 17.4.12. Let timingInfo's fetch event dispatch time to the coarsened shared current time given
    //                 useHighResPerformanceTimers.

    // 17.4.13. Dispatch e at activeWorker's global object.
    // NB: The execution context must not stay on the stack while the event is dispatched, so that a microtask
    //     checkpoint can be performed after each listener.
    global.dispatch_event(&cpp_event);

    with_temporary_execution_context(realm, || {
        // 17.4.14. Invoke Update Service Worker Extended Events Set with activeWorker and e.
        let extended_event_id = update_service_worker_extended_events_set(vm, event.extendable_event.lifetime());

        // 17.4.15. If e's respond-with entered flag is set, set respondWithEntered to true.
        // 17.4.19. If e's canceled flag is set, set eventCanceled to true.
        // FIXME: 17.4.20. If fetchController state is "terminated" or "aborted", then:
        let dispatched = DispatchedFetchEvent {
            respond_with_entered: event.respond_with_entered_flag.get(),
            event_canceled: cpp_event.canceled(),
            ctx,
            on_settled,
        };

        // 17.4.16. If e's wait to respond flag is set, then:
        //          1. Wait until e's wait to respond flag is unset.
        if event.wait_to_respond_flag.get() {
            event.waiting_dispatch.set(Some(dispatched));

            // NB: An event that waits is active, so it is in the set of extended events, whose deadline ends the wait
            //     if the flag is still set by then.
            if let Some(id) = extended_event_id {
                event.waiting_extended_event_id.set(Some(id));
                FETCH_EVENTS_WAITING_TO_RESPOND
                    .with_borrow_mut(|events| events.push((id, event, CellRoot::new(event))));
            }
        } else {
            event.finish_create_fetch_event_and_dispatch(vm, dispatched, false);
        }
        extended_event_id
    })
}

/// Ends the wait of the fetch event that is in the set of extended events with the id, if its deadline passed before
/// its wait to respond flag was unset.
pub(crate) fn time_out_fetch_event(vm: &Vm, extended_event_id: u64) {
    let Some((_, event, _root)) = FETCH_EVENTS_WAITING_TO_RESPOND.with_borrow_mut(|events| {
        let index = events.iter().position(|(id, _, _)| *id == extended_event_id)?;
        Some(events.remove(index))
    }) else {
        return;
    };
    event.waiting_extended_event_id.set(None);
    let Some(dispatched) = event.waiting_dispatch.take() else {
        return;
    };

    // https://w3c.github.io/ServiceWorker/#service-worker-lifetime
    // The user agent may terminate service workers when it detects abnormal operation, such as tasks exceeding imposed
    // time limits while handling the events.
    // NB: Terminating the worker would discard the event's task, after which Create Fetch Event and Dispatch returns a
    //     network error. The event ends as if its task were discarded, and the worker keeps handling its other events.
    //     A respondWith() promise that settles afterwards changes nothing.
    with_temporary_execution_context_and_callbacks(promise_realm(event.handled), || {
        event.finish_create_fetch_event_and_dispatch(vm, dispatched, true);
    });
}

/// Dispatches a fetch event for the request at the global object, whose EventTarget `global` is, and calls
/// `on_settled` with `ctx` once Create Fetch Event and Dispatch knows what it returns. Returns the id that the event is
/// in the set of extended events with, or 0 if the event is not in it.
///
/// # Safety
///
/// The pointers must be the live VM, a live realm, the realm's live global EventTarget and the wrapper of a Request
/// object of the realm. The client ids must be lent for the call. `on_settled` must be callable with `ctx` until it is
/// called, and `on_pending_events_changed` with `pending_events_ctx` for as long as the process runs the worker.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn web_rust_service_worker_dispatch_fetch_event(
    vm: *mut c_void,
    realm: *mut c_void,
    global: *mut c_void,
    request: *mut c_void,
    client_id: ak::Span<'_, u8>,
    resulting_client_id: ak::Span<'_, u8>,
    replaces_client_id: ak::Span<'_, u8>,
    ctx: *mut c_void,
    on_settled: OnFetchEventSettled,
    pending_events_ctx: *mut c_void,
    on_pending_events_changed: OnPendingEventsChanged,
) -> u64 {
    // SAFETY: The caller passes the live VM, a live realm, its global EventTarget and the wrapper of a Request object.
    let (vm, realm, global, request) = unsafe {
        let (vm, realm) = realm_arguments(vm, realm);
        (
            vm,
            realm,
            CppEventTarget::from_raw(global),
            CppInterface::from_wrapper(Gc::from_non_null(
                NonNull::new(request.cast::<Object>()).expect("C++ passes the wrapper of a Request object"),
            )),
        )
    };
    let string = |span: ak::Span<'_, u8>| Utf16String::from_utf8(&String::from_utf8_lossy(span.as_slice()));
    let client_ids = FetchEventClientIds {
        client_id: string(client_id),
        resulting_client_id: string(resulting_client_id),
        replaces_client_id: string(replaces_client_id),
    };
    set_on_pending_events_changed(pending_events_ctx, on_pending_events_changed);
    dispatch_fetch_event(vm, realm, global, request, client_ids, ctx, on_settled).unwrap_or(0)
}
