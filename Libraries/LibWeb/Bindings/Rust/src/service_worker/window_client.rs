/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! WindowClient, https://w3c.github.io/ServiceWorker/#windowclient-interface

use ak::Utf16String;
use libjs_rust::api::{Array, Gc, IntegrityLevel, MarkedVec, Must, Object, PrimitiveString, Realm, Trace, Value, Vm};

use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::document::DocumentVisibilityState;
use crate::service_worker::client::{Client, ServiceWorkerClientSnapshot};

// https://w3c.github.io/ServiceWorker/#windowclient-interface
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct WindowClient {
    client: Client,

    // A WindowClient object has an associated visibility state, which is one of visibilityState attribute value.
    #[gc(untraced)]
    visibility_state: DocumentVisibilityState,

    // A WindowClient object has an associated focus state, which is either true or false (initially false).
    #[gc(untraced)]
    focus_state: bool,

    // A WindowClient object has an associated ancestor origins array.
    ancestor_origins_array: Gc<Object>,
}

impl Wrappable for WindowClient {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        self.client.relevant_realm()
    }
}

impl Inherits<Client> for WindowClient {
    fn parent(&self) -> &Client {
        &self.client
    }
}

impl WindowClient {
    // https://w3c.github.io/ServiceWorker/#create-windowclient-algorithm
    pub(crate) fn create(vm: &Vm, realm: Gc<Realm>, client: &ServiceWorkerClientSnapshot) -> Ref<Self> {
        // 6. Set windowClient's ancestor origins array to a frozen array created from ancestorOriginsList.
        let ancestor_origins = MarkedVec::new(vm);
        for origin in &client.ancestor_origins {
            ancestor_origins.push(Value::from_string(PrimitiveString::create(
                vm,
                Utf16String::from_utf8(origin),
            )));
        }
        let ancestor_origins_array = Array::create_from_list(vm, realm, &ancestor_origins);
        ancestor_origins_array
            .set_integrity_level(vm, IntegrityLevel::Frozen)
            .must();

        // 1. Let windowClient be a new WindowClient object.
        // 2. Set windowClient's service worker client to client.
        // 3. Set windowClient's frame type to frameType.
        // 4. Set windowClient's visibility state to visibilityState.
        // 5. Set windowClient's focus state to focusState.
        // 7. Return windowClient.
        allocate(vm, |_| Self {
            client: Client::new(realm, client, client.frame_type),
            visibility_state: client.visibility_state,
            focus_state: client.focused,
            ancestor_origins_array: ancestor_origins_array.upcast::<Object>(),
        })
    }

    // https://w3c.github.io/ServiceWorker/#client-visibilitystate
    pub(crate) fn visibility_state(&self, _: &mut CallContext) -> WebIdlResult<DocumentVisibilityState> {
        // The visibilityState getter steps are to return this's visibility state.
        Ok(self.visibility_state)
    }

    // https://w3c.github.io/ServiceWorker/#client-focused
    pub(crate) fn focused(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        // The focused getter steps are to return this's focus state.
        Ok(self.focus_state)
    }

    // https://w3c.github.io/ServiceWorker/#client-ancestororigins
    pub(crate) fn ancestor_origins(&self, _: &mut CallContext) -> WebIdlResult<Gc<Object>> {
        // The ancestorOrigins getter steps are to return this's associated ancestor origins array.
        Ok(self.ancestor_origins_array)
    }
}
