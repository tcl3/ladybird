/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Clients, https://w3c.github.io/ServiceWorker/#clients-interface

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{
    Array, EmbedderCell, EmbedderClass, EmbedderValue, Gc, IntegrityLevel, MarkedVec, Must, Object, Realm, Trace,
    Value, Vm,
};

use crate::bindings::interface::{Ref, Wrappable, allocate, with_temporary_execution_context_and_callbacks, wrap};
use crate::bindings::promise::{
    TaskSource, WebIdlPromise, create_promise, promise_realm, queue_a_global_task, resolve_promise,
};
use crate::bindings::{CallContext, WebIdlResult};
use crate::service_worker::client::{AnyClient, Client, ServiceWorkerClientSnapshot};
use crate::service_worker::service_worker_container::{
    ServiceWorkerRequestKind, add_pending_request, take_pending_request,
};
use crate::service_worker::window_client::WindowClient;

unsafe extern "C" {
    /// Asks the UI process for the service worker clients that the request matches: the one with the id, or those of
    /// the type, controlled or not as the request says.
    fn web_service_worker_request_clients(
        realm: *mut c_void,
        request_id: u64,
        client_id: ak::Span<'_, u8>,
        is_get: bool,
        include_uncontrolled: bool,
        client_type: u8,
    );
}

// https://w3c.github.io/ServiceWorker/#enumdef-clienttype
/// In the order of Web::Bindings::ClientType, which C++ passes as its underlying value.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum ClientType {
    Window,
    Worker,
    Sharedworker,
    All,
}

impl ClientType {
    pub(crate) fn from_u8(value: u8) -> Option<Self> {
        match value {
            0 => Some(Self::Window),
            1 => Some(Self::Worker),
            2 => Some(Self::Sharedworker),
            3 => Some(Self::All),
            _ => None,
        }
    }
}

/// The dictionary ClientQueryOptions of ServiceWorker/Clients.idl, whose members the bindings convert.
pub(crate) struct ClientQueryOptions {
    pub(crate) include_uncontrolled: bool,
    pub(crate) r#type: ClientType,
}

// https://w3c.github.io/ServiceWorker/#clients-interface
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Clients {
    realm: Gc<Realm>,
}

impl Wrappable for Clients {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.realm)
    }
}

impl Clients {
    pub(crate) fn create(vm: &Vm, realm: Gc<Realm>) -> Ref<Self> {
        allocate(vm, |_| Self { realm })
    }

    fn request(
        &self,
        vm: &Vm,
        kind: ServiceWorkerRequestKind,
        client_id: &str,
        options: ClientQueryOptions,
    ) -> WebIdlPromise {
        let promise = create_promise(vm, self.realm);
        let request_id = add_pending_request(vm, promise, kind);
        // SAFETY: The realm is live, and the id is lent for the call.
        unsafe {
            web_service_worker_request_clients(
                self.realm.as_ptr().cast(),
                request_id,
                ak::Span::new(client_id.as_bytes()),
                kind == ServiceWorkerRequestKind::ClientsGet,
                options.include_uncontrolled,
                options.r#type as u8,
            );
        }
        promise
    }

    // https://w3c.github.io/ServiceWorker/#clients-get
    pub(crate) fn get(&self, cx: &mut CallContext, id: Utf16String) -> WebIdlResult<WebIdlPromise> {
        // 1. Let promise be a new promise.
        // 2. Run these substeps in parallel:
        //    1. For each service worker client client where the result of running obtain a storage key given client
        //       equals the associated service worker's containing service worker registration's storage key:
        //       1. If client's id is not id, continue.
        //       2. Wait for either client's execution ready flag to be set or for client's discarded flag to be set.
        //       3. If client's execution ready flag is set, queue a task on promise's relevant settings object's
        //          responsible event loop, using the DOM manipulation task source, to run the following steps:
        //          1. Invoke Resolve Get Client Promise with client and promise.
        //          2. Abort these steps.
        //    2. Queue a task on promise's relevant settings object's responsible event loop, using the DOM
        //       manipulation task source, to resolve promise with undefined.
        // NB: The UI process finds the client, which service_worker_clients_matched() resolves the promise with.
        // FIXME: Wait for a client whose execution ready flag is unset.
        let id = String::from_utf16_lossy(&id.to_utf16());
        let options = ClientQueryOptions {
            include_uncontrolled: true,
            r#type: ClientType::All,
        };

        // 3. Return promise.
        Ok(self.request(cx.vm(), ServiceWorkerRequestKind::ClientsGet, &id, options))
    }

    // https://w3c.github.io/ServiceWorker/#clients-matchall
    pub(crate) fn match_all(&self, cx: &mut CallContext, options: ClientQueryOptions) -> WebIdlResult<WebIdlPromise> {
        // 1. Let promise be a new promise.
        // 2. Run the following steps in parallel:
        // NB: The UI process finds the clients that match, which service_worker_clients_matched() resolves the promise
        //     with.

        // 3. Return promise.
        Ok(self.request(cx.vm(), ServiceWorkerRequestKind::ClientsMatchAll, "", options))
    }
}

/// The clients that the UI process found for a request of a Clients object.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct MatchedClients {
    promise: WebIdlPromise,
    #[gc(untraced)]
    is_get: bool,
    #[gc(untraced)]
    clients: Vec<ServiceWorkerClientSnapshot>,
}

static MATCHED_CLIENTS_CLASS: EmbedderClass<MatchedClients> = EmbedderClass::new("ServiceWorkerMatchedClients");

impl EmbedderValue for MatchedClients {
    const CLASS: &'static EmbedderClass<Self> = &MATCHED_CLIENTS_CLASS;
}

fn create_client_object(vm: &Vm, realm: Gc<Realm>, client: &ServiceWorkerClientSnapshot) -> AnyClient {
    if client.r#type == ClientType::Window {
        AnyClient::WindowClient(WindowClient::create(vm, realm, client))
    } else {
        AnyClient::Client(Client::create(vm, realm, client))
    }
}

fn wrap_client(cx: &CallContext, client: AnyClient) -> Gc<Object> {
    match client {
        AnyClient::Client(client) => wrap(cx, client),
        AnyClient::WindowClient(window_client) => wrap(cx, window_client),
    }
}

pub(crate) fn service_worker_clients_matched(vm: &Vm, request_id: u64, clients: Vec<ServiceWorkerClientSnapshot>) {
    let (request, is_get) =
        if let Some((request, _)) = take_pending_request(request_id, ServiceWorkerRequestKind::ClientsGet) {
            (request, true)
        } else if let Some((request, _)) = take_pending_request(request_id, ServiceWorkerRequestKind::ClientsMatchAll) {
            (request, false)
        } else {
            return;
        };
    let promise = request.promise;
    let matched = MATCHED_CLIENTS_CLASS.allocate(vm, |_| MatchedClients {
        promise,
        is_get,
        clients,
    });

    queue_a_global_task(
        vm,
        promise_realm(promise).global_object(),
        TaskSource::DOMManipulation,
        matched,
        |vm, matched: Gc<EmbedderCell<MatchedClients>>| {
            let realm = promise_realm(matched.promise);
            with_temporary_execution_context_and_callbacks(realm, || {
                let cx = CallContext::new(vm, realm);
                if matched.is_get {
                    // https://w3c.github.io/ServiceWorker/#resolve-get-client-promise
                    let Some(client) = matched.clients.first() else {
                        // 2.2. Queue a task on promise's relevant settings object's responsible event loop, using the
                        //      DOM manipulation task source, to resolve promise with undefined.
                        resolve_promise(vm, matched.promise, Value::UNDEFINED);
                        return;
                    };
                    let client_object = create_client_object(vm, realm, client);
                    resolve_promise(vm, matched.promise, Value::from_object(wrap_client(&cx, client_object)));
                    return;
                }

                // https://w3c.github.io/ServiceWorker/#clients-matchall
                // 2.6.1. Let clientObjects be a new list.
                // 2.6.2. For each windowData in matchedWindowData:
                //        1. Let windowClient be the result of running Create Window Client algorithm with
                //           windowData["client"], windowData["frameType"], windowData["visibilityState"],
                //           windowData["focusState"], and windowData["ancestorOriginsList"] as the arguments.
                //        2. Append windowClient to clientObjects.
                // 2.6.3. For each client in matchedClients:
                //        1. Let clientObject be the result of running Create Client algorithm with client as the
                //           argument.
                //        2. Append clientObject to clientObjects.
                // 2.6.4. Sort clientObjects such that:
                // NB: The UI process sorts the clients.
                let client_objects = MarkedVec::new(vm);
                for client in &matched.clients {
                    let client_object = create_client_object(vm, realm, client);
                    client_objects.push(Value::from_object(wrap_client(&cx, client_object)));
                }

                // 2.6.8. Resolve promise with a new frozen array of clientObjects in promise's relevant Realm.
                let array = Array::create_from_list(vm, realm, &client_objects);
                array.set_integrity_level(vm, IntegrityLevel::Frozen).must();
                resolve_promise(vm, matched.promise, Value::from_object(array.upcast::<Object>()));
            });
        },
    );
}
