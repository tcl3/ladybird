/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! WindowClient, https://w3c.github.io/ServiceWorker/#windowclient-interface

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{
    Array, EmbedderCell, EmbedderClass, EmbedderValue, Gc, IntegrityLevel, MarkedVec, Must, Object, PrimitiveString,
    Realm, Trace, Value, Vm,
};
use liburl_rust::url::{ExcludeFragment, Url};

use crate::bindings::conversions::UsvString;
use crate::bindings::interface::{
    Inherits, Ref, Wrappable, allocate, with_temporary_execution_context_and_callbacks, wrap,
};
use crate::bindings::promise::{
    TaskSource, WebIdlPromise, create_promise, create_rejected_promise_from_exception, promise_realm,
    queue_a_global_task, reject_promise_with_exception, resolve_promise,
};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::dom::document::DocumentVisibilityState;
use crate::service_worker::client::{Client, ServiceWorkerClientSnapshot};
use crate::service_worker::service_worker_container::{
    ServiceWorkerRequestKind, add_pending_request, parse_with_base, service_worker_client, take_pending_request,
};

unsafe extern "C" {
    /// Asks the UI process to navigate the window client with the id to the URL, for the service worker of the realm's
    /// global object.
    fn web_service_worker_navigate_client(
        realm: *mut c_void,
        request_id: u64,
        client_id: ak::Span<'_, u8>,
        url: ak::Span<'_, u8>,
    );
}

// https://url.spec.whatwg.org/#matches-about-blank
fn matches_about_blank(url: &Url) -> bool {
    // A URL matches about:blank if its scheme is "about", its path contains a single string "blank", its username and
    // password are the empty string, and its host is null.
    url.scheme() == "about"
        && url.serialize_path() == "blank"
        && url.username().is_empty()
        && url.password().is_empty()
        && url.serialized_host().is_none()
}

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

    // https://w3c.github.io/ServiceWorker/#client-navigate
    pub(crate) fn navigate(&self, cx: &mut CallContext, url: UsvString) -> WebIdlResult<WebIdlPromise> {
        let vm = cx.vm();
        let realm = self.client.realm();

        // 1. Let url be the result of parsing url with this's relevant settings object's API base URL.
        let url = parse_with_base(&url, &service_worker_client(realm).api_base_url);

        // 2. If url is failure, return a promise rejected with a TypeError.
        let Some(url) = url else {
            return Ok(create_rejected_promise_from_exception(
                vm,
                realm,
                Exception::TypeError(Utf16String::from_utf8("The URL is not valid")),
            ));
        };

        // 3. If url is about:blank, return a promise rejected with a TypeError.
        if matches_about_blank(&url) {
            return Ok(create_rejected_promise_from_exception(
                vm,
                realm,
                Exception::TypeError(Utf16String::from_utf8(
                    "A window client cannot be navigated to about:blank",
                )),
            ));
        }

        // 4. If this's associated service worker client's active service worker is not this's relevant global object's
        //    service worker, return a promise rejected with a TypeError.
        // NB: The UI process, which knows the client's active service worker, rejects the promise.

        // 5. Let serviceWorkerEventLoop be the current global object's event loop.
        // 6. Let promise be a new promise.
        let promise = create_promise(vm, realm);
        let request_id = add_pending_request(vm, promise, ServiceWorkerRequestKind::ClientNavigate);

        // 7. Queue a task to run the following steps on this's associated service worker client's responsible event
        //    loop using the user interaction task source:
        // NB: The client's process runs the steps, and service_worker_client_navigated() settles the promise.
        let client_id = self.client.id_string();
        // SAFETY: The realm is live, and the strings are lent for the call.
        unsafe {
            web_service_worker_navigate_client(
                realm.as_ptr().cast(),
                request_id,
                ak::Span::new(client_id.as_bytes()),
                ak::Span::new(url.serialize(ExcludeFragment::No).as_bytes()),
            );
        }

        // 8. Return promise.
        Ok(promise)
    }
}

/// The errors that a navigation that WindowClient.navigate() asked for can reject its promise with.
#[derive(Clone, Copy)]
pub(crate) enum ClientNavigationError {
    TypeError,
    SecurityError,
}

/// How a navigation that WindowClient.navigate() asked for ended.
pub(crate) enum ClientNavigationOutcome {
    Rejected(ClientNavigationError),
    Navigated(Option<ServiceWorkerClientSnapshot>),
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct ClientNavigationAnswer {
    promise: WebIdlPromise,
    #[gc(untraced)]
    outcome: ClientNavigationOutcome,
}

static CLIENT_NAVIGATION_ANSWER_CLASS: EmbedderClass<ClientNavigationAnswer> =
    EmbedderClass::new("ServiceWorkerClientNavigationAnswer");

impl EmbedderValue for ClientNavigationAnswer {
    const CLASS: &'static EmbedderClass<Self> = &CLIENT_NAVIGATION_ANSWER_CLASS;
}

pub(crate) fn service_worker_client_navigated(vm: &Vm, request_id: u64, outcome: ClientNavigationOutcome) {
    let Some((request, _root)) = take_pending_request(request_id, ServiceWorkerRequestKind::ClientNavigate) else {
        return;
    };
    let answer = CLIENT_NAVIGATION_ANSWER_CLASS.allocate(vm, |_| ClientNavigationAnswer {
        promise: request.promise,
        outcome,
    });

    // https://w3c.github.io/ServiceWorker/#client-navigate
    // 7.9. Queue a task to run the following steps on serviceWorkerEventLoop using the DOM manipulation task source:
    queue_a_global_task(
        vm,
        promise_realm(answer.promise).global_object(),
        TaskSource::DOMManipulation,
        answer,
        |vm, answer: Gc<EmbedderCell<ClientNavigationAnswer>>| {
            let realm = promise_realm(answer.promise);
            with_temporary_execution_context_and_callbacks(realm, || match &answer.outcome {
                ClientNavigationOutcome::Rejected(error) => {
                    let exception = match error {
                        ClientNavigationError::TypeError => {
                            Exception::TypeError(Utf16String::from_utf8("The window client could not be navigated"))
                        }
                        ClientNavigationError::SecurityError => Exception::Dom(
                            "SecurityError",
                            Utf16String::from_utf8("The window client is not allowed to navigate to the URL"),
                        ),
                    };
                    reject_promise_with_exception(vm, answer.promise, exception);
                }
                // 1. If browsingContext's Window object's environment settings object's creation URL's origin is not
                //    the same as the service worker's origin, resolve promise with null and abort these steps.
                ClientNavigationOutcome::Navigated(None) => resolve_promise(vm, answer.promise, Value::NULL),
                // 2. Let windowClient be the result of running Create Window Client with this's service worker client,
                //    frameType, visibilityState, focusState, and ancestorOriginsList.
                // 3. Resolve promise with windowClient.
                ClientNavigationOutcome::Navigated(Some(client)) => {
                    let cx = CallContext::new(vm, realm);
                    let window_client = WindowClient::create(vm, realm, client);
                    resolve_promise(vm, answer.promise, Value::from_object(wrap(&cx, window_client)));
                }
            });
        },
    );
}
