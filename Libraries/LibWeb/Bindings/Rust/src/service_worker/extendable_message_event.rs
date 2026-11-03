/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! ExtendableMessageEvent, https://w3c.github.io/ServiceWorker/#extendablemessageevent-interface

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Array, Gc, IntegrityLevel, MarkedVec, Must, Object, Realm, Trace, Value, Vm};

use crate::bindings::CallContext;
use crate::bindings::WebIdlResult;
use crate::bindings::conversions::UsvString;
use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate, wrap};
use crate::html::message_port::MessagePort;
use crate::service_worker::client::AnyClient;
use crate::service_worker::extendable_event::{ExtendableEvent, ExtendableEventInit};
use crate::service_worker::service_worker::ServiceWorker;

/// The union (Client or ServiceWorker or MessagePort) of ServiceWorker/ExtendableMessageEvent.idl, whose members the
/// bindings convert.
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum ClientOrServiceWorkerOrMessagePort {
    Client(AnyClient),
    ServiceWorker(Ref<ServiceWorker>),
    MessagePort(Ref<MessagePort>),
}

/// The dictionary ExtendableMessageEventInit of ServiceWorker/ExtendableMessageEvent.idl, whose members the bindings
/// convert.
pub(crate) struct ExtendableMessageEventInit<'vm> {
    pub(crate) parent: ExtendableEventInit,
    pub(crate) data: Value,
    pub(crate) origin: UsvString,
    pub(crate) last_event_id: Utf16String,
    pub(crate) source: Option<ClientOrServiceWorkerOrMessagePort>,
    pub(crate) ports: MarkedVec<'vm, Ref<MessagePort>>,
}

// https://w3c.github.io/ServiceWorker/#extendablemessageevent-interface
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ExtendableMessageEvent {
    extendable_event: ExtendableEvent,
    data: Value,

    // Each ExtendableMessageEvent has an origin (an origin, a string, or null), initially null.
    // NB: The origin of an event that the user agent creates is serialized when the event is created.
    #[gc(untraced)]
    origin: UsvString,
    #[gc(untraced)]
    last_event_id: Utf16String,
    source: Option<ClientOrServiceWorkerOrMessagePort>,
    ports: Gc<Object>,
}

impl Wrappable for ExtendableMessageEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.extendable_event.cpp_parent()
    }
}

impl Inherits<ExtendableEvent> for ExtendableMessageEvent {
    fn parent(&self) -> &ExtendableEvent {
        &self.extendable_event
    }
}

/// What a new ExtendableMessageEvent is initialized with, besides its ports.
pub(crate) struct ExtendableMessageEventFields {
    pub(crate) data: Value,
    pub(crate) origin: UsvString,
    pub(crate) last_event_id: Utf16String,
    pub(crate) source: Option<ClientOrServiceWorkerOrMessagePort>,
}

impl ExtendableMessageEvent {
    pub(crate) fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_type: &Utf16String,
        event_init: &ExtendableEventInit,
        fields: ExtendableMessageEventFields,
        ports: &MarkedVec<'_, Value>,
    ) -> Ref<Self> {
        let event = CppEvent::create_for_owner(
            realm,
            event_type,
            event_init.parent.bubbles,
            event_init.parent.cancelable,
            event_init.parent.composed,
        );
        let ports = Array::create_from_list(vm, realm, ports);
        ports.set_integrity_level(vm, IntegrityLevel::Frozen).must();
        let object = allocate(vm, |_| Self {
            extendable_event: ExtendableEvent::new(vm, event),
            data: fields.data,
            origin: fields.origin,
            last_event_id: fields.last_event_id,
            source: fields.source,
            ports: ports.upcast::<Object>(),
        });
        object.extendable_event.event().set_owner(object);
        object
    }

    // https://w3c.github.io/ServiceWorker/#dom-extendablemessageevent-extendablemessageevent
    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: ExtendableMessageEventInit<'_>,
    ) -> WebIdlResult<Ref<Self>> {
        let ports = MarkedVec::new(cx.vm());
        for port in event_init_dict.ports.values().iter() {
            ports.push(Value::from_object(wrap(cx, *port)));
        }
        Ok(Self::create(
            cx.vm(),
            cx.realm(),
            &r#type,
            &event_init_dict.parent,
            ExtendableMessageEventFields {
                data: event_init_dict.data,
                origin: event_init_dict.origin,
                last_event_id: event_init_dict.last_event_id,
                source: event_init_dict.source,
            },
            &ports,
        ))
    }

    // https://w3c.github.io/ServiceWorker/#dom-extendablemessageevent-data
    pub(crate) fn data(&self, _: &mut CallContext) -> WebIdlResult<Value> {
        // The data attribute must return the value it was initialized to.
        Ok(self.data)
    }

    // https://w3c.github.io/ServiceWorker/#dom-extendablemessageevent-origin
    pub(crate) fn origin(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        // 1. If this's origin is an origin, then return the serialization of this's origin.
        // 2. If this's origin is null, then return the empty string.
        // 3. Return this's origin.
        Ok(self.origin.clone())
    }

    // https://w3c.github.io/ServiceWorker/#dom-extendablemessageevent-lasteventid
    pub(crate) fn last_event_id(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The lastEventId attribute must return the value it was initialized to.
        Ok(self.last_event_id.clone())
    }

    // https://w3c.github.io/ServiceWorker/#dom-extendablemessageevent-source
    pub(crate) fn source(&self, _: &mut CallContext) -> WebIdlResult<Option<ClientOrServiceWorkerOrMessagePort>> {
        // The source attribute must return the value it was initialized to.
        Ok(self.source.clone())
    }

    // https://w3c.github.io/ServiceWorker/#dom-extendablemessageevent-ports
    pub(crate) fn ports(&self, _: &mut CallContext) -> WebIdlResult<Gc<Object>> {
        // The ports attribute must return the value it was initialized to.
        Ok(self.ports)
    }
}
