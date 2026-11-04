/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI of the service worker interfaces for LibWeb's C++ code: Libraries/LibWeb/ServiceWorker/RustIntegration.cpp.

use core::ffi::c_void;

use libjs_rust::api::Vm;

use crate::bindings::interface::{Ref, cell_from_abi, cell_into_abi, realm_arguments};
use crate::dom::document::DocumentVisibilityState;
use crate::html::structured_serialize::cpp::OwnedSerializedTransferRecord;
use crate::service_worker::client::{FrameType, ServiceWorkerClientSnapshot};
use crate::service_worker::clients::{ClientType, Clients, service_worker_clients_matched};
use crate::service_worker::navigation_preload_manager::navigation_preload_settled;
use crate::service_worker::service_worker::{
    ServiceWorker, ServiceWorkerState, get_the_service_worker_object, service_worker_state_changed,
};
use crate::service_worker::service_worker_container::{
    ServiceWorkerContainer, ServiceWorkerJobResult, add_client_message, enable_client_message_queue,
    service_worker_job_settled, service_worker_ready, service_worker_registration_lookup_finished,
};
use crate::service_worker::service_worker_registration::get_the_service_worker_registration_object;
use crate::service_worker::service_worker_registration::{
    ServiceWorkerRegistrationSlot, ServiceWorkerUpdateViaCache, service_worker_registration_state_changed,
    service_worker_update_found,
};
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

/// A Web::ServiceWorker::ServiceWorkerClientSnapshot that C++ lends.
#[repr(C)]
pub struct FfiServiceWorkerClientSnapshot<'a> {
    id: ak::Span<'a, u8>,
    url: ak::Span<'a, u8>,
    r#type: u8,
    frame_type: u8,
    visibility_state: u8,
    focused: bool,
    ancestor_origins: ak::Span<'a, ak::Span<'a, u8>>,
}

fn string_from_abi(string: ak::Span<'_, u8>) -> String {
    String::from_utf8_lossy(string.as_slice()).into_owned()
}

pub(crate) fn descriptor_from_abi(descriptor: &FfiServiceWorkerDescriptor<'_>) -> Option<ServiceWorkerDescriptor> {
    descriptor.exists.then(|| ServiceWorkerDescriptor {
        id: descriptor.id,
        script_url: string_from_abi(descriptor.script_url),
        state: ServiceWorkerState::from_u8(descriptor.state).expect("C++ passes a ServiceWorkerState"),
    })
}

pub(crate) fn client_snapshot_from_abi(client: &FfiServiceWorkerClientSnapshot<'_>) -> ServiceWorkerClientSnapshot {
    ServiceWorkerClientSnapshot {
        id: string_from_abi(client.id),
        url: string_from_abi(client.url),
        r#type: ClientType::from_u8(client.r#type).expect("C++ passes a ClientType"),
        frame_type: FrameType::from_u8(client.frame_type).expect("C++ passes a FrameType"),
        visibility_state: DocumentVisibilityState::from_u8(client.visibility_state)
            .expect("C++ passes a DocumentVisibilityState"),
        focused: client.focused,
        ancestor_origins: client
            .ancestor_origins
            .as_slice()
            .iter()
            .map(|origin| string_from_abi(*origin))
            .collect(),
    }
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

/// The UI process set a slot of a registration to the worker, if `worker` exists, or to null.
///
/// # Safety
///
/// `vm` must be the live VM, and the worker must be lent for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_service_worker_registration_state_changed(
    vm: *mut c_void,
    registration_id: u64,
    target: u8,
    worker: *const FfiServiceWorkerDescriptor<'_>,
) {
    // SAFETY: The caller passes the live VM and lends the worker.
    let (vm, worker) = unsafe { (&*vm.cast::<Vm>(), descriptor_from_abi(&*worker)) };
    let target = ServiceWorkerRegistrationSlot::from_u8(target).expect("C++ passes a ServiceWorkerRegistrationSlot");
    service_worker_registration_state_changed(vm, registration_id, target, worker);
}

/// The UI process set the state of a service worker.
///
/// # Safety
///
/// `vm` must be the live VM, and the worker must be lent for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_service_worker_state_changed(
    vm: *mut c_void,
    worker: *const FfiServiceWorkerDescriptor<'_>,
) {
    // SAFETY: The caller passes the live VM and lends the worker.
    let (vm, worker) = unsafe { (&*vm.cast::<Vm>(), descriptor_from_abi(&*worker)) };
    service_worker_state_changed(vm, &worker.expect("C++ passes a worker"));
}

/// The UI process found an update of a registration, which it is installing.
///
/// # Safety
///
/// `vm` must be the live VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_service_worker_update_found(vm: *mut c_void, registration_id: u64) {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { &*vm.cast::<Vm>() };
    service_worker_update_found(vm, registration_id);
}

/// The UI process found the registration that resolves a ready promise of this process.
///
/// # Safety
///
/// `vm` must be the live VM, and the registration must be lent for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_service_worker_ready(
    vm: *mut c_void,
    request_id: u64,
    registration: *const FfiServiceWorkerRegistrationSnapshot<'_>,
) {
    // SAFETY: The caller passes the live VM and lends the registration.
    let (vm, registration) = unsafe { (&*vm.cast::<Vm>(), snapshot_from_abi(&*registration)) };
    service_worker_ready(vm, request_id, registration);
}

/// A service worker posted a message to the service worker client whose realm this is, which the client message queue
/// of the client's ServiceWorkerContainer dispatches.
///
/// # Safety
///
/// The pointers must be the live VM and a live realm, the source and the origin must be lent for the call, and LibWeb
/// hands `record` over.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_service_worker_message_to_client(
    vm: *mut c_void,
    realm: *mut c_void,
    source: *const FfiServiceWorkerDescriptor<'_>,
    origin: ak::Span<'_, u8>,
    record: *mut c_void,
) {
    // SAFETY: The caller passes the live VM and a live realm, lends the source, and hands the record over.
    let (vm, realm, source, record) = unsafe {
        let (vm, realm) = realm_arguments(vm, realm);
        (
            vm,
            realm,
            descriptor_from_abi(&*source),
            OwnedSerializedTransferRecord::from_raw(record),
        )
    };
    add_client_message(
        vm,
        realm,
        source.expect("C++ passes a service worker"),
        string_from_abi(origin),
        record,
    );
}

/// Enables the client message queue of the ServiceWorkerContainer whose service worker client's realm this is.
///
/// # Safety
///
/// The pointers must be the live VM and a live realm.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_service_worker_enable_client_message_queue(vm: *mut c_void, realm: *mut c_void) {
    // SAFETY: The caller passes the live VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    enable_client_message_queue(vm, realm);
}

/// The ServiceWorkerRegistration object that represents the registration in the realm.
///
/// # Safety
///
/// The pointers must be the live VM and a live realm, and the registration must be lent for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_service_worker_registration_object(
    vm: *mut c_void,
    realm: *mut c_void,
    registration: *const FfiServiceWorkerRegistrationSnapshot<'_>,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live realm, and lends the registration.
    let (vm, realm, registration) = unsafe {
        let (vm, realm) = realm_arguments(vm, realm);
        (vm, realm, snapshot_from_abi(&*registration))
    };
    cell_into_abi(get_the_service_worker_registration_object(vm, realm, &registration))
}

/// The ServiceWorker object that represents the service worker in the realm.
///
/// # Safety
///
/// The pointers must be the live VM and a live realm, and the worker must be lent for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_service_worker_service_worker_object(
    vm: *mut c_void,
    realm: *mut c_void,
    worker: *const FfiServiceWorkerDescriptor<'_>,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live realm, and lends the worker.
    let (vm, realm, worker) = unsafe {
        let (vm, realm) = realm_arguments(vm, realm);
        (
            vm,
            realm,
            descriptor_from_abi(&*worker).expect("C++ passes a service worker"),
        )
    };
    cell_into_abi(get_the_service_worker_object(vm, realm, &worker))
}

/// The id of the service worker that the ServiceWorker object represents.
///
/// # Safety
///
/// `service_worker` must be the live cell of a ServiceWorker object.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_service_worker_id(service_worker: *mut c_void) -> u64 {
    // SAFETY: The caller passes the cell of a ServiceWorker object.
    let service_worker: Ref<ServiceWorker> = unsafe { cell_from_abi(service_worker) };
    service_worker.id()
}

/// The UI process ran the navigation preload request of this process, and gives the registration's state, which
/// `enabled` and `header_value` are, if `succeeded` is true.
///
/// # Safety
///
/// `vm` must be the live VM, and the header value must be lent for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_service_worker_navigation_preload_settled(
    vm: *mut c_void,
    request_id: u64,
    succeeded: bool,
    enabled: bool,
    header_value: ak::Span<'_, u8>,
) {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { &*vm.cast::<Vm>() };
    let state = succeeded.then(|| (enabled, header_value.as_slice().to_vec()));
    navigation_preload_settled(vm, request_id, state);
}

/// The UI process found the clients that a request of a Clients object of this process matches.
///
/// # Safety
///
/// `vm` must be the live VM, and the clients must be lent for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_service_worker_clients_matched(
    vm: *mut c_void,
    request_id: u64,
    clients: ak::Span<'_, FfiServiceWorkerClientSnapshot<'_>>,
) {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { &*vm.cast::<Vm>() };
    let clients = clients.as_slice().iter().map(client_snapshot_from_abi).collect();
    service_worker_clients_matched(vm, request_id, clients);
}

/// The Clients object of the service worker global scope whose realm this is.
///
/// # Safety
///
/// The pointers must be the live VM and a live realm.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_service_worker_create_clients(vm: *mut c_void, realm: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    cell_into_abi(Clients::create(vm, realm))
}
