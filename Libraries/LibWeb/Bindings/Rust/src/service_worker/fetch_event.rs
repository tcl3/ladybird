/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! FetchEvent, https://w3c.github.io/ServiceWorker/#fetchevent-interface

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Value, Vm};

use crate::bindings::conversions::CppInterface;
use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::promise::{WebIdlPromise, create_promise, create_resolved_promise};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::service_worker::extendable_event::{ExtendableEvent, ExtendableEventInit, add_lifetime_promise};

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
    // FIXME: Keep the potential response, and the wait to respond and respond-with error flags, which only Handle
    //        Fetch reads.
    #[gc(untraced)]
    respond_with_entered_flag: Cell<bool>,
}

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
            respond_with_entered_flag: Cell::new(false),
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
    pub(crate) fn respond_with(&self, cx: &mut CallContext, r: WebIdlPromise) -> WebIdlResult<()> {
        // 1. Let event be this.
        let event = self.extendable_event.event();

        // 2. If event's dispatch flag is unset, throw an "InvalidStateError" DOMException.
        if !event.dispatched() {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_utf8("respondWith() was called on an event that is not being dispatched"),
            ));
        }

        // 3. If event's respond-with entered flag is set, throw an "InvalidStateError" DOMException.
        if self.respond_with_entered_flag.get() {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_utf8("respondWith() was already called on the event"),
            ));
        }

        // 4. Add lifetime promise r to event.
        // NOTE: event.respondWith(r) extends the lifetime of the event by default as if event.waitUntil(r) is called.
        add_lifetime_promise(cx.vm(), self.extendable_event.lifetime(), r)?;

        // 5. Set event's stop propagation flag and stop immediate propagation flag.
        event.stop_immediate_propagation();

        // 6. Set event's respond-with entered flag.
        self.respond_with_entered_flag.set(true);

        // FIXME: 7. Set event's wait to respond flag.
        // FIXME: 8. Let targetRealm be event's relevant Realm.
        // FIXME: 9. Upon rejection of r:
        //           1. Set event's respond-with error flag.
        //           2. Unset event's wait to respond flag.
        // FIXME: 10. Upon fulfillment of r with response:
        //            1. If response is not a Response object, then set the respond-with error flag.
        //            2. Else:
        // NB: The substeps of step 10.2 read response's body into event's potential response, which Handle Fetch
        //     returns.
        Ok(())
    }
}
