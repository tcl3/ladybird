/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! ServiceWorker, https://w3c.github.io/ServiceWorker/#serviceworker-interface

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::conversions::UsvString;
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::realm_data::realm_data;
use crate::bindings::{CallContext, WebIdlResult};
use crate::service_worker::ServiceWorkerDescriptor;

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

impl ServiceWorker {
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

    // 3. Set objectMap[serviceWorker] to serviceWorkerObj.
    data.service_worker_objects.borrow_mut(vm).push(service_worker_object);

    // 3. Return objectMap[serviceWorker].
    service_worker_object
}
