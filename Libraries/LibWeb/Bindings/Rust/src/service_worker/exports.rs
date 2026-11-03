/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI of the service worker interfaces for LibWeb's C++ code: Libraries/LibWeb/ServiceWorker/RustIntegration.cpp.

use core::ffi::c_void;

use libjs_rust::api::Vm;

use crate::bindings::interface::{cell_into_abi, realm_arguments};
use crate::service_worker::service_worker::ServiceWorkerState;
use crate::service_worker::service_worker_container::{
    ServiceWorkerContainer, ServiceWorkerJobResult, service_worker_job_settled,
    service_worker_registration_lookup_finished,
};
use crate::service_worker::service_worker_registration::ServiceWorkerUpdateViaCache;
use crate::service_worker::{ServiceWorkerDescriptor, ServiceWorkerRegistrationSnapshot};

/// A Web::ServiceWorker::ServiceWorkerDescriptor that C++ lends, if `exists` is true.
#[repr(C)]
pub struct FfiServiceWorkerDescriptor<'a> {
    exists: bool,
    id: u64,
    script_url: ak::Span<'a, u8>,
    state: u8,
}

/// A Web::ServiceWorker::ServiceWorkerRegistrationSnapshot that C++ lends.
#[repr(C)]
pub struct FfiServiceWorkerRegistrationSnapshot<'a> {
    id: u64,
    scope_url: ak::Span<'a, u8>,
    update_via_cache: u8,
    installing_worker: FfiServiceWorkerDescriptor<'a>,
    waiting_worker: FfiServiceWorkerDescriptor<'a>,
    active_worker: FfiServiceWorkerDescriptor<'a>,
}

fn string_from_abi(string: ak::Span<'_, u8>) -> String {
    String::from_utf8_lossy(string.as_slice()).into_owned()
}

fn descriptor_from_abi(descriptor: &FfiServiceWorkerDescriptor<'_>) -> Option<ServiceWorkerDescriptor> {
    descriptor.exists.then(|| ServiceWorkerDescriptor {
        id: descriptor.id,
        script_url: string_from_abi(descriptor.script_url),
        state: ServiceWorkerState::from_u8(descriptor.state).expect("C++ passes a ServiceWorkerState"),
    })
}

fn snapshot_from_abi(snapshot: &FfiServiceWorkerRegistrationSnapshot<'_>) -> ServiceWorkerRegistrationSnapshot {
    ServiceWorkerRegistrationSnapshot {
        id: snapshot.id,
        scope_url: string_from_abi(snapshot.scope_url),
        update_via_cache: ServiceWorkerUpdateViaCache::from_u8(snapshot.update_via_cache)
            .expect("C++ passes a ServiceWorkerUpdateViaCache"),
        installing_worker: descriptor_from_abi(&snapshot.installing_worker),
        waiting_worker: descriptor_from_abi(&snapshot.waiting_worker),
        active_worker: descriptor_from_abi(&snapshot.active_worker),
    }
}

/// # Safety
///
/// The pointers must be the live VM and a live realm.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_service_worker_create_container(vm: *mut c_void, realm: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    cell_into_abi(ServiceWorkerContainer::create(vm, realm))
}

/// The job of a request of this process settled. `kind` is 0 for a registration, which `registration` lends, 1 for
/// the boolean, 2 for a TypeError and 3 for a "SecurityError" DOMException.
///
/// # Safety
///
/// `vm` must be the live VM, and `registration` must be null or lent for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_service_worker_job_settled(
    vm: *mut c_void,
    request_id: u64,
    kind: u8,
    registration: *const FfiServiceWorkerRegistrationSnapshot<'_>,
    boolean: bool,
) {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { &*vm.cast::<Vm>() };
    let result = match kind {
        // SAFETY: The caller lends the registration.
        0 => ServiceWorkerJobResult::Registration(snapshot_from_abi(unsafe { &*registration })),
        1 => ServiceWorkerJobResult::Boolean(boolean),
        2 => ServiceWorkerJobResult::TypeError,
        3 => ServiceWorkerJobResult::SecurityError,
        _ => unreachable!("C++ passes a kind of job result"),
    };
    service_worker_job_settled(vm, request_id, result);
}

/// The registrations that a request of this process for registrations found, which the caller lends.
///
/// # Safety
///
/// `vm` must be the live VM, and the registrations must be lent for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_service_worker_registration_lookup_finished(
    vm: *mut c_void,
    request_id: u64,
    registrations: ak::Span<'_, FfiServiceWorkerRegistrationSnapshot<'_>>,
) {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { &*vm.cast::<Vm>() };
    let registrations = registrations.as_slice().iter().map(snapshot_from_abi).collect();
    service_worker_registration_lookup_finished(vm, request_id, registrations);
}
