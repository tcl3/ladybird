/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! InstallEvent, https://w3c.github.io/ServiceWorker/#installevent-interface

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::service_worker::extendable_event::{ExtendableEvent, ExtendableEventInit};

// https://w3c.github.io/ServiceWorker/#installevent-interface
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct InstallEvent {
    extendable_event: ExtendableEvent,
}

impl Wrappable for InstallEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.extendable_event.cpp_parent()
    }
}

impl Inherits<ExtendableEvent> for InstallEvent {
    fn parent(&self) -> &ExtendableEvent {
        &self.extendable_event
    }
}

impl InstallEvent {
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
        let object = allocate(vm, |_| Self {
            extendable_event: ExtendableEvent::new(vm, event),
        });
        object.extendable_event.event().set_owner(object);
        object
    }

    // https://w3c.github.io/ServiceWorker/#dom-installevent-installevent
    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: ExtendableEventInit,
    ) -> WebIdlResult<Ref<Self>> {
        Ok(Self::create(cx.vm(), cx.realm(), &r#type, &event_init_dict))
    }
}
