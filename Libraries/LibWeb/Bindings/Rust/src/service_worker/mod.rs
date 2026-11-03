/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Service Workers, https://w3c.github.io/ServiceWorker/. The UI process holds the registrations, service workers and
//! job queues of every process, and describes them to the realms that have objects for them.

pub(crate) mod client;
pub(crate) mod clients;
pub(crate) mod exports;
pub(crate) mod extendable_event;
pub(crate) mod extendable_message_event;
pub(crate) mod install_event;
pub(crate) mod lifecycle_events;
pub(crate) mod messages;
#[allow(
    clippy::module_inception,
    reason = "the bindings name the module of an interface after its IDL file"
)]
pub(crate) mod service_worker;
pub(crate) mod service_worker_container;
pub(crate) mod service_worker_registration;
pub(crate) mod window_client;

use libjs_rust::api::{EmbedderCell, GcWeak, MarkedVec, Vm};

use crate::bindings::interface::{Interface, Ref, Wrapped};
use crate::service_worker::service_worker::ServiceWorkerState;
use crate::service_worker::service_worker_registration::ServiceWorkerUpdateViaCache;

/// Lets go of what the service worker code of an agent that is going away keeps outside its heap.
pub(crate) fn shut_down_service_workers_of_agent() {
    service_worker_container::forget_pending_requests_of_agent();
    service_worker::forget_service_worker_objects_of_agent();
    service_worker_registration::forget_registration_objects_of_agent();
    lifecycle_events::forget_lifecycle_event_of_agent();
    messages::forget_extended_events_of_agent();
}

/// A service worker, https://w3c.github.io/ServiceWorker/#dfn-service-worker, as the UI process describes it.
#[derive(Clone)]
pub(crate) struct ServiceWorkerDescriptor {
    pub(crate) id: u64,
    pub(crate) script_url: String,
    pub(crate) state: ServiceWorkerState,
}

/// A service worker registration, https://w3c.github.io/ServiceWorker/#dfn-service-worker-registration, as the UI
/// process describes it.
#[derive(Clone)]
pub(crate) struct ServiceWorkerRegistrationSnapshot {
    pub(crate) id: u64,
    pub(crate) scope_url: String,
    pub(crate) update_via_cache: ServiceWorkerUpdateViaCache,
    pub(crate) installing_worker: Option<ServiceWorkerDescriptor>,
    pub(crate) waiting_worker: Option<ServiceWorkerDescriptor>,
    pub(crate) active_worker: Option<ServiceWorkerDescriptor>,
}

/// The objects of an interface that the realms of this thread's agent hold, by references that do not keep them alive,
/// which a collected object leaves empty. The empty ones are pruned once the list has grown to twice what was live
/// after the last pruning.
pub(crate) struct LiveObjects<T: Interface> {
    objects: Vec<GcWeak<EmbedderCell<Wrapped<T>>>>,
    live_after_pruning: usize,
}

impl<T: Interface> LiveObjects<T> {
    const OBJECTS_BEFORE_FIRST_PRUNING: usize = 64;

    pub(crate) const fn new() -> Self {
        Self {
            objects: Vec::new(),
            live_after_pruning: 0,
        }
    }

    pub(crate) fn clear(&mut self) {
        self.objects.clear();
        self.live_after_pruning = 0;
    }

    pub(crate) fn add(&mut self, vm: &Vm, object: Ref<T>) {
        if self.objects.len() >= (2 * self.live_after_pruning).max(Self::OBJECTS_BEFORE_FIRST_PRUNING) {
            self.objects.retain(|object| object.get().is_some());
            self.live_after_pruning = self.objects.len();
        }
        self.objects.push(GcWeak::new(vm.heap(), object));
    }

    pub(crate) fn matching<'vm>(&self, vm: &'vm Vm, predicate: impl Fn(&T) -> bool) -> MarkedVec<'vm, Ref<T>> {
        let objects = MarkedVec::new(vm);
        for object in self.objects.iter().filter_map(GcWeak::get) {
            if predicate(&object) {
                objects.push(object);
            }
        }
        objects
    }
}
