/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Client, https://w3c.github.io/ServiceWorker/#client-interface

use core::ffi::c_void;
use core::ops::Deref;

use ak::Utf16String;
use libjs_rust::api::{Gc, Object, Realm, ThrowCompletionOr, Trace, Value, Vm};

use crate::bindings::conversions::{ToJs, UsvString};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::document::DocumentVisibilityState;
use crate::html::message_port::StructuredSerializeOptions;
use crate::html::structured_serialize::structured_serialize_with_transfer;
use crate::html::structured_serialize::transfer_record::TransferRecord;
use crate::service_worker::clients::ClientType;
use crate::service_worker::window_client::WindowClient;

unsafe extern "C" {
    /// Sends the message to the service worker client with the id, for its process to add a task to the client
    /// message queue of the client's ServiceWorkerContainer. LibWeb takes the record over.
    fn web_service_worker_post_message_to_client(realm: *mut c_void, client_id: ak::Span<'_, u8>, record: *mut c_void);
}

// https://w3c.github.io/ServiceWorker/#enumdef-frametype
/// In the order of Web::Bindings::FrameType, which C++ passes as its underlying value.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum FrameType {
    Auxiliary,
    TopLevel,
    Nested,
    None,
}

impl FrameType {
    pub(crate) fn from_u8(value: u8) -> Option<Self> {
        match value {
            0 => Some(Self::Auxiliary),
            1 => Some(Self::TopLevel),
            2 => Some(Self::Nested),
            3 => Some(Self::None),
            _ => None,
        }
    }
}

/// What the UI process says of a service worker client, Web::ServiceWorker::ServiceWorkerClientSnapshot, which a
/// Client or WindowClient object represents.
pub(crate) struct ServiceWorkerClientSnapshot {
    pub(crate) id: String,
    pub(crate) url: String,
    pub(crate) r#type: ClientType,
    pub(crate) frame_type: FrameType,
    pub(crate) visibility_state: DocumentVisibilityState,
    pub(crate) focused: bool,
    pub(crate) ancestor_origins: Vec<String>,
}

// https://w3c.github.io/ServiceWorker/#client-interface
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Client {
    realm: Gc<Realm>,

    // A Client object has an associated service worker client (a service worker client).
    #[gc(untraced)]
    id: Utf16String,
    #[gc(untraced)]
    url: UsvString,
    #[gc(untraced)]
    r#type: ClientType,

    // A Client object has an associated frame type, which is one of "auxiliary", "top-level", "nested", and "none".
    // Unless stated otherwise it is "none".
    #[gc(untraced)]
    frame_type: FrameType,
}

impl Wrappable for Client {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.realm)
    }
}

/// An object of Client or of WindowClient, which inherits from it.
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum AnyClient {
    Client(Ref<Client>),
    WindowClient(Ref<WindowClient>),
}

impl Deref for AnyClient {
    type Target = Client;

    fn deref(&self) -> &Client {
        match self {
            Self::Client(client) => client,
            Self::WindowClient(window_client) => {
                let window_client: &WindowClient = window_client;
                window_client.parent()
            }
        }
    }
}

impl ToJs for AnyClient {
    fn to_js(self, cx: &CallContext) -> ThrowCompletionOr<Value> {
        match self {
            Self::Client(client) => client.to_js(cx),
            Self::WindowClient(window_client) => window_client.to_js(cx),
        }
    }
}

impl Client {
    pub(crate) fn new(realm: Gc<Realm>, client: &ServiceWorkerClientSnapshot, frame_type: FrameType) -> Self {
        Self {
            realm,
            id: Utf16String::from_utf8(&client.id),
            url: UsvString::from_utf8(&client.url),
            r#type: client.r#type,
            frame_type,
        }
    }

    pub(crate) fn realm(&self) -> Gc<Realm> {
        self.realm
    }

    /// The id of the object's service worker client.
    pub(crate) fn id_string(&self) -> String {
        String::from_utf16_lossy(&self.id.to_utf16())
    }

    // https://w3c.github.io/ServiceWorker/#create-client-algorithm
    pub(crate) fn create(vm: &Vm, realm: Gc<Realm>, client: &ServiceWorkerClientSnapshot) -> Ref<Self> {
        // 1. Let clientObject be a new Client object.
        // 2. Set clientObject's service worker client to client.
        // 3. Return clientObject.
        allocate(vm, |_| Self::new(realm, client, FrameType::None))
    }

    // https://w3c.github.io/ServiceWorker/#client-url
    pub(crate) fn url(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        // The url getter steps are to return this's associated service worker client's serialized creation URL.
        Ok(self.url.clone())
    }

    // https://w3c.github.io/ServiceWorker/#client-frametype
    pub(crate) fn frame_type(&self, _: &mut CallContext) -> WebIdlResult<FrameType> {
        // The frameType getter steps are to return this's frame type.
        Ok(self.frame_type)
    }

    // https://w3c.github.io/ServiceWorker/#client-id
    pub(crate) fn id(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The id getter steps are to return this's associated service worker client's id.
        Ok(self.id.clone())
    }

    // https://w3c.github.io/ServiceWorker/#client-type
    pub(crate) fn r#type(&self, _: &mut CallContext) -> WebIdlResult<ClientType> {
        // 1. Let client be this's service worker client.
        // 2. If client is an environment settings object, then:
        //    1. If client is a window client, return "window".
        //    2. Else if client is a dedicated worker client, return "worker".
        //    3. Else if client is a shared worker client, return "sharedworker".
        // 3. Else:
        //    1. Return "window".
        // NB: The UI process says which kind of client the client is.
        Ok(self.r#type)
    }

    // https://w3c.github.io/ServiceWorker/#client-postmessage-transfer
    pub(crate) fn post_message(
        &self,
        cx: &mut CallContext,
        message: Value,
        transfer: libjs_rust::api::MarkedVec<'_, Gc<Object>>,
    ) -> WebIdlResult<()> {
        // 1. Let options be «[ "transfer" → transfer ]».
        // 2. Invoke postMessage(message, options) with message and options.
        self.post_message_steps(cx, message, &transfer.values())
    }

    // https://w3c.github.io/ServiceWorker/#client-postmessage-options
    pub(crate) fn post_message_with_options(
        &self,
        cx: &mut CallContext,
        message: Value,
        options: StructuredSerializeOptions<'_>,
    ) -> WebIdlResult<()> {
        self.post_message_steps(cx, message, &options.transfer.values())
    }

    // https://w3c.github.io/ServiceWorker/#client-postmessage-options
    fn post_message_steps(&self, cx: &CallContext, message: Value, transfer: &[Gc<Object>]) -> WebIdlResult<()> {
        // 1. Let contextObject be this.
        // 2. Let sourceSettings be the contextObject's relevant settings object.

        // 3. Let serializeWithTransferResult be StructuredSerializeWithTransfer(message, options["transfer"]). Rethrow
        //    any exceptions.
        let serialize_with_transfer_result = structured_serialize_with_transfer(cx, message, transfer)?;

        // 4. Run the following steps in parallel:
        // NB: The UI process finds the target client, and its process adds the task to the client message queue.
        let id = String::from_utf16_lossy(&self.id.to_utf16());
        // SAFETY: The realm is live, the id is lent for the call, and LibWeb takes the record over.
        unsafe {
            web_service_worker_post_message_to_client(
                self.realm.as_ptr().cast(),
                ak::Span::new(id.as_bytes()),
                TransferRecord::into_cpp(serialize_with_transfer_result, cx.vm()).into_raw(),
            );
        }
        Ok(())
    }
}
