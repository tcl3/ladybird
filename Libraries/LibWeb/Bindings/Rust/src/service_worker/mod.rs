/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Service Workers, https://w3c.github.io/ServiceWorker/. The UI process holds the registrations, service workers and
//! job queues of every process, and describes them to the realms that have objects for them.

pub(crate) mod exports;
#[allow(
    clippy::module_inception,
    reason = "the bindings name the module of an interface after its IDL file"
)]
pub(crate) mod service_worker;
pub(crate) mod service_worker_container;
pub(crate) mod service_worker_registration;

use crate::service_worker::service_worker::ServiceWorkerState;
use crate::service_worker::service_worker_registration::ServiceWorkerUpdateViaCache;

/// Lets go of what the service worker code of an agent that is going away keeps outside its heap.
pub(crate) fn shut_down_service_workers_of_agent() {
    service_worker_container::forget_pending_requests_of_agent();
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
