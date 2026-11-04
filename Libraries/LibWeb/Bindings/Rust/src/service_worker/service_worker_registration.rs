/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! ServiceWorkerRegistration, https://w3c.github.io/ServiceWorker/#serviceworkerregistration-interface

use core::cell::{Cell, RefCell};
use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::conversions::UsvString;
use crate::bindings::event_target::{CppEvent, CppEventTarget, GetTheParent};
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::promise::{WebIdlPromise, create_promise, create_rejected_promise_from_exception};
use crate::bindings::realm_data::realm_data;
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::html::worker::WorkerType;
use crate::service_worker::navigation_preload_manager::NavigationPreloadManager;
use crate::service_worker::service_worker::{ServiceWorker, get_the_service_worker_object};
use crate::service_worker::service_worker_container::{ServiceWorkerJobType, schedule_job};
use crate::service_worker::{LiveObjects, ServiceWorkerDescriptor, ServiceWorkerRegistrationSnapshot};

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

unsafe extern "C" {
    /// Whether the realm's global object is a ServiceWorkerGlobalScope whose service worker is installing.
    fn web_service_worker_global_scope_is_installing(realm: *mut c_void) -> bool;
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

    // https://w3c.github.io/ServiceWorker/#dom-serviceworkerregistration-navigationpreload
    navigation_preload: Cell<Option<Ref<NavigationPreloadManager>>>,
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

thread_local! {
    static REGISTRATION_OBJECTS: RefCell<LiveObjects<ServiceWorkerRegistration>> =
        const { RefCell::new(LiveObjects::new()) };
}

/// Lets go of the objects of an agent that is going away, whose weak references would outlive its heap.
pub(crate) fn forget_registration_objects_of_agent() {
    REGISTRATION_OBJECTS.with_borrow_mut(LiveObjects::clear);
}

/// In the order of Web::ServiceWorker::ServiceWorkerRegistrationSlot, which C++ passes as its underlying value.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum ServiceWorkerRegistrationSlot {
    Installing,
    Waiting,
    Active,
}

impl ServiceWorkerRegistrationSlot {
    pub(crate) fn from_u8(value: u8) -> Option<Self> {
        match value {
            0 => Some(Self::Installing),
            1 => Some(Self::Waiting),
            2 => Some(Self::Active),
            _ => None,
        }
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

    // https://w3c.github.io/ServiceWorker/#dom-serviceworkerregistration-navigationpreload
    pub(crate) fn navigation_preload(&self, cx: &mut CallContext) -> WebIdlResult<Ref<NavigationPreloadManager>> {
        // The navigationPreload getter steps are to return the service worker registration's
        // NavigationPreloadManager object.
        if let Some(manager) = self.navigation_preload.get() {
            return Ok(manager);
        }
        let manager = NavigationPreloadManager::create(cx.vm(), self.realm, self.id);
        self.navigation_preload.set(Some(manager));
        Ok(manager)
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

    // https://w3c.github.io/ServiceWorker/#service-worker-registration-update
    pub(crate) fn update(&self, cx: &mut CallContext) -> WebIdlResult<WebIdlPromise> {
        let vm = cx.vm();

        // 1. Let registration be the service worker registration.
        // 2. Let newestWorker be the result of running Get Newest Worker algorithm passing registration as its
        //    argument.
        // NB: The UI process gets the newest worker again when the job runs. This one names its script url.
        let newest_worker = self.installing.get().or(self.waiting.get()).or(self.active.get());

        // 3. If newestWorker is null, return a promise rejected with an "InvalidStateError" DOMException and abort these
        //    steps.
        let Some(newest_worker) = newest_worker else {
            return Ok(create_rejected_promise_from_exception(
                vm,
                self.realm,
                Exception::Dom(
                    "InvalidStateError",
                    Utf16String::from_utf8("The registration has no service worker to update"),
                ),
            ));
        };

        // 4. If this's relevant global object globalObject is a ServiceWorkerGlobalScope object, and globalObject's
        //    associated service worker's state is "installing", return a promise rejected with an "InvalidStateError"
        //    DOMException and abort these steps.
        // SAFETY: The realm is live.
        if unsafe { web_service_worker_global_scope_is_installing(self.realm.as_ptr().cast()) } {
            return Ok(create_rejected_promise_from_exception(
                vm,
                self.realm,
                Exception::Dom(
                    "InvalidStateError",
                    Utf16String::from_utf8("An installing service worker cannot update its registration"),
                ),
            ));
        }

        // 5. Let promise be a promise.
        let promise = create_promise(vm, self.realm);

        // 6. Let job be the result of running Create Job with update, registration's storage key, registration's scope
        //    url, newestWorker's script url, promise, and this's relevant settings object.
        // 7. Set job's worker type to newestWorker's type.
        // 8. Invoke Schedule Job with job.
        // NB: The UI process sets the job's worker type to the newest worker's type.
        schedule_job(
            vm,
            self.realm,
            promise,
            ServiceWorkerJobType::Update,
            &self.scope_url.to_utf8(),
            Some(&newest_worker.script_url_string()),
            WorkerType::Classic,
            self.update_via_cache.get(),
        );

        // 9. Return promise.
        Ok(promise)
    }

    // https://w3c.github.io/ServiceWorker/#navigator-service-worker-unregister
    pub(crate) fn unregister(&self, cx: &mut CallContext) -> WebIdlResult<WebIdlPromise> {
        // NOTE: The unregister() method unregisters the service worker registration. It is important to note that the
        //       currently controlled service worker client's active service worker's containing service worker
        //       registration is effective until all the service worker clients (including itself) using this service
        //       worker registration unload. That is, the unregister() method only affects subsequent navigations.

        // 1. Let registration be the service worker registration.
        // 2. Let promise be a new promise.
        let promise = create_promise(cx.vm(), self.realm);

        // 3. Let job be the result of running Create Job with unregister, registration's storage key, registration's
        //    scope url, null, promise, and this's relevant settings object.
        // 4. Invoke Schedule Job with job.
        schedule_job(
            cx.vm(),
            self.realm,
            promise,
            ServiceWorkerJobType::Unregister,
            &self.scope_url.to_utf8(),
            None,
            WorkerType::Classic,
            self.update_via_cache.get(),
        );

        // 5. Return promise.
        Ok(promise)
    }
}

// https://w3c.github.io/ServiceWorker/#update-registration-state-algorithm
pub(crate) fn service_worker_registration_state_changed(
    vm: &Vm,
    registration_id: u64,
    target: ServiceWorkerRegistrationSlot,
    source: Option<ServiceWorkerDescriptor>,
) {
    // 2. For each registrationObject in registrationObjects:
    //    1. Queue a task to set the installing attribute of registrationObject to null if registration's installing
    //       worker is null, or the result of getting the service worker object that represents registration's
    //       installing worker in registrationObject's relevant settings object.
    // NB: The waiting and active attributes are set in the same way. The task that sets them is the one of the message
    //     from the UI process, which sets them for each realm of this process.
    let objects =
        REGISTRATION_OBJECTS.with_borrow(|objects| objects.matching(vm, |object| object.id == registration_id));
    for registration_object in objects.values().iter() {
        let worker = source
            .as_ref()
            .map(|source| get_the_service_worker_object(vm, registration_object.realm, source));
        let attribute = match target {
            ServiceWorkerRegistrationSlot::Installing => &registration_object.installing,
            ServiceWorkerRegistrationSlot::Waiting => &registration_object.waiting,
            ServiceWorkerRegistrationSlot::Active => &registration_object.active,
        };
        attribute.set(worker);
    }
}

// https://w3c.github.io/ServiceWorker/#installation-algorithm
pub(crate) fn service_worker_update_found(vm: &Vm, registration_id: u64) {
    // 9. For each settingsObject of settingsObjects, queue a task on settingsObject's responsible event loop in the DOM
    //    manipulation task source to run the following steps:
    // NB: The task that runs these steps is the one of the message from the UI process, which runs them for each realm
    //     of this process.
    // 1. Let registrationObjects be every ServiceWorkerRegistration object in settingsObject's realm, whose service
    //    worker registration is registration.
    let objects =
        REGISTRATION_OBJECTS.with_borrow(|objects| objects.matching(vm, |object| object.id == registration_id));

    // 2. For each registrationObject of registrationObjects, fire an event on registrationObject named updatefound.
    for registration_object in objects.values().iter() {
        let event = CppEvent::create(registration_object.realm, "updatefound", false, false, false);
        event.set_is_trusted(true);
        registration_object.event_target.dispatch_event(&event);
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
        // NB: The UI process describes the registration as it is now, whose update via cache mode an update or a
        //     register job can have changed.
        registration_object.update_via_cache.set(registration.update_via_cache);

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
        navigation_preload: Cell::new(None),
    });
    registration_object.event_target.set_owner(registration_object);
    REGISTRATION_OBJECTS.with_borrow_mut(|objects| objects.add(vm, registration_object));

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
