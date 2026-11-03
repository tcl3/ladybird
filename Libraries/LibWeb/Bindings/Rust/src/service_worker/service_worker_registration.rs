/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! ServiceWorkerRegistration, https://w3c.github.io/ServiceWorker/#serviceworkerregistration-interface

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::conversions::UsvString;
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::realm_data::realm_data;
use crate::bindings::{CallContext, WebIdlResult};
use crate::service_worker::ServiceWorkerRegistrationSnapshot;
use crate::service_worker::service_worker::{ServiceWorker, get_the_service_worker_object};

// https://w3c.github.io/ServiceWorker/#enumdef-serviceworkerupdateviacache
/// In the order of Web::Bindings::ServiceWorkerUpdateViaCache, which C++ passes as its underlying value.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum ServiceWorkerUpdateViaCache {
    #[default]
    Imports,
    All,
    None,
}

impl ServiceWorkerUpdateViaCache {
    pub(crate) fn from_u8(value: u8) -> Option<Self> {
        match value {
            0 => Some(Self::Imports),
            1 => Some(Self::All),
            2 => Some(Self::None),
            _ => None,
        }
    }
}

// https://w3c.github.io/ServiceWorker/#serviceworkerregistration-interface
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ServiceWorkerRegistration {
    event_target: CppEventTarget,
    realm: Gc<Realm>,

    // A ServiceWorkerRegistration has a service worker registration (a service worker registration).
    id: u64,
    #[gc(untraced)]
    scope_url: UsvString,
    #[gc(untraced)]
    update_via_cache: Cell<ServiceWorkerUpdateViaCache>,

    installing: Cell<Option<Ref<ServiceWorker>>>,
    waiting: Cell<Option<Ref<ServiceWorker>>>,
    active: Cell<Option<Ref<ServiceWorker>>>,
}

impl Wrappable for ServiceWorkerRegistration {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.realm)
    }

    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event_target.as_cpp_parent()
    }
}

impl GetTheParent for ServiceWorkerRegistration {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

impl ServiceWorkerRegistration {
    // https://w3c.github.io/ServiceWorker/#dom-serviceworkerregistration-installing
    pub(crate) fn installing(&self, _: &mut CallContext) -> WebIdlResult<Option<Ref<ServiceWorker>>> {
        // installing attribute must return the value to which it was last set.
        Ok(self.installing.get())
    }

    // https://w3c.github.io/ServiceWorker/#dom-serviceworkerregistration-waiting
    pub(crate) fn waiting(&self, _: &mut CallContext) -> WebIdlResult<Option<Ref<ServiceWorker>>> {
        // waiting attribute must return the value to which it was last set.
        Ok(self.waiting.get())
    }

    // https://w3c.github.io/ServiceWorker/#dom-serviceworkerregistration-active
    pub(crate) fn active(&self, _: &mut CallContext) -> WebIdlResult<Option<Ref<ServiceWorker>>> {
        // active attribute must return the value to which it was last set.
        Ok(self.active.get())
    }

    // https://w3c.github.io/ServiceWorker/#dom-serviceworkerregistration-scope
    pub(crate) fn scope(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        // The scope getter steps are to return the service worker registration's serialized scope url.
        Ok(self.scope_url.clone())
    }

    // https://w3c.github.io/ServiceWorker/#dom-serviceworkerregistration-updateviacache
    pub(crate) fn update_via_cache(&self, _: &mut CallContext) -> WebIdlResult<ServiceWorkerUpdateViaCache> {
        // The updateViaCache getter steps are to return the service worker registration's update via cache mode.
        Ok(self.update_via_cache.get())
    }
}

// https://w3c.github.io/ServiceWorker/#get-the-service-worker-registration-object
pub(crate) fn get_the_service_worker_registration_object(
    vm: &Vm,
    realm: Gc<Realm>,
    registration: &ServiceWorkerRegistrationSnapshot,
) -> Ref<ServiceWorkerRegistration> {
    // 1. Let objectMap be environment's service worker registration object map.
    let data = realm_data(vm, realm);
    if let Some(registration_object) = data
        .service_worker_registration_objects
        .borrow()
        .iter()
        .find(|object| object.id == registration.id)
    {
        // 3. Return objectMap[registration].
        return *registration_object;
    }

    // 2. If objectMap[registration] does not exist, then:
    // 1. Let registrationObject be a new ServiceWorkerRegistration in environment's Realm.
    // 2. Set registrationObject's service worker registration to registration.
    // 3. Set registrationObject's installing attribute to null.
    // 4. Set registrationObject's waiting attribute to null.
    // 5. Set registrationObject's active attribute to null.
    let registration_object = allocate(vm, |_| ServiceWorkerRegistration {
        event_target: CppEventTarget::new(),
        realm,
        id: registration.id,
        scope_url: UsvString::from_utf8(&registration.scope_url),
        update_via_cache: Cell::new(registration.update_via_cache),
        installing: Cell::new(None),
        waiting: Cell::new(None),
        active: Cell::new(None),
    });
    registration_object.event_target.set_owner(registration_object);

    // 6. If registration's installing worker is not null, then set registrationObject's installing attribute to the
    //    result of getting the service worker object that represents registration's installing worker in environment.
    if let Some(installing_worker) = &registration.installing_worker {
        registration_object
            .installing
            .set(Some(get_the_service_worker_object(vm, realm, installing_worker)));
    }

    // 7. If registration's waiting worker is not null, then set registrationObject's waiting attribute to the result
    //    of getting the service worker object that represents registration's waiting worker in environment.
    if let Some(waiting_worker) = &registration.waiting_worker {
        registration_object
            .waiting
            .set(Some(get_the_service_worker_object(vm, realm, waiting_worker)));
    }

    // 8. If registration's active worker is not null, then set registrationObject's active attribute to the result of
    //    getting the service worker object that represents registration's active worker in environment.
    if let Some(active_worker) = &registration.active_worker {
        registration_object
            .active
            .set(Some(get_the_service_worker_object(vm, realm, active_worker)));
    }

    // 9. Set objectMap[registration] to registrationObject.
    data.service_worker_registration_objects
        .borrow_mut(vm)
        .push(registration_object);

    // 3. Return objectMap[registration].
    registration_object
}
