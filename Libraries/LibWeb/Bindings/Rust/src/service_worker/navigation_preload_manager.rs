/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! NavigationPreloadManager, https://w3c.github.io/ServiceWorker/#navigationpreloadmanager

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{EmbedderCell, EmbedderClass, EmbedderValue, Gc, Must, PropertyKey, Realm, Trace, Value, Vm};

use crate::bindings::conversions::{ByteString, ToJs};
use crate::bindings::interface::{
    Ref, Wrappable, allocate, create_dictionary_object, set_dictionary_member,
    with_temporary_execution_context_and_callbacks,
};
use crate::bindings::promise::{
    TaskSource, WebIdlPromise, create_promise, create_rejected_promise_from_exception, promise_realm,
    queue_a_global_task, reject_promise_with_exception, resolve_promise,
};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::service_worker::service_worker_container::{
    ServiceWorkerRequestKind, add_pending_request, take_pending_request,
};

unsafe extern "C" {
    /// Asks the UI process to run the operation on the registration's navigation preload state.
    fn web_service_worker_request_navigation_preload(
        realm: *mut c_void,
        request_id: u64,
        registration_id: u64,
        operation: u8,
        header_value: ak::Span<'_, u8>,
    );
}

/// In the order of Web::ServiceWorker::ServiceWorkerNavigationPreloadOperation, which C++ passes as its underlying
/// value.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
enum NavigationPreloadOperation {
    Enable,
    Disable,
    SetHeaderValue,
    GetState,
}

/// The dictionary NavigationPreloadState of ServiceWorker/NavigationPreloadManager.idl, whose members the bindings
/// convert.
pub(crate) struct NavigationPreloadState {
    pub(crate) enabled: bool,
    pub(crate) header_value: Option<ByteString>,
}

// https://w3c.github.io/ServiceWorker/#navigationpreloadmanager
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct NavigationPreloadManager {
    realm: Gc<Realm>,

    // NB: The id of the associated service worker registration, which the UI process holds.
    #[gc(untraced)]
    registration_id: u64,
}

impl Wrappable for NavigationPreloadManager {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.realm)
    }
}

// https://fetch.spec.whatwg.org/#http-whitespace-byte
fn is_http_whitespace_byte(byte: u8) -> bool {
    matches!(byte, b'\t' | b'\n' | b'\r' | b' ')
}

impl NavigationPreloadManager {
    pub(crate) fn create(vm: &Vm, realm: Gc<Realm>, registration_id: u64) -> Ref<Self> {
        allocate(vm, |_| Self { realm, registration_id })
    }

    fn request(&self, vm: &Vm, operation: NavigationPreloadOperation, header_value: &[u8]) -> WebIdlPromise {
        // 1. Let promise be a new promise.
        let promise = create_promise(vm, self.realm);

        // 2. Run the following steps in parallel:
        // NB: The UI process holds the registration, and runs the steps.
        let kind = if operation == NavigationPreloadOperation::GetState {
            ServiceWorkerRequestKind::NavigationPreloadState
        } else {
            ServiceWorkerRequestKind::NavigationPreloadUpdate
        };
        let request_id = add_pending_request(vm, promise, kind);
        // SAFETY: The realm is live, and the header value is lent for the call.
        unsafe {
            web_service_worker_request_navigation_preload(
                self.realm.as_ptr().cast(),
                request_id,
                self.registration_id,
                operation as u8,
                ak::Span::new(header_value),
            );
        }

        // 3. Return promise.
        promise
    }

    // https://w3c.github.io/ServiceWorker/#dom-navigationpreloadmanager-enable
    pub(crate) fn enable(&self, cx: &mut CallContext) -> WebIdlResult<WebIdlPromise> {
        Ok(self.request(cx.vm(), NavigationPreloadOperation::Enable, &[]))
    }

    // https://w3c.github.io/ServiceWorker/#dom-navigationpreloadmanager-disable
    pub(crate) fn disable(&self, cx: &mut CallContext) -> WebIdlResult<WebIdlPromise> {
        Ok(self.request(cx.vm(), NavigationPreloadOperation::Disable, &[]))
    }

    // https://w3c.github.io/ServiceWorker/#dom-navigationpreloadmanager-setheadervalue
    pub(crate) fn set_header_value(&self, cx: &mut CallContext, value: ByteString) -> WebIdlResult<WebIdlPromise> {
        // 1. Let value be the result of normalizing value.
        // https://fetch.spec.whatwg.org/#concept-header-value-normalize
        // To normalize a byte sequence potentialValue, remove any leading and trailing HTTP whitespace bytes from
        // potentialValue.
        let bytes = value.bytes();
        let start = bytes.iter().position(|byte| !is_http_whitespace_byte(*byte));
        let end = bytes.iter().rposition(|byte| !is_http_whitespace_byte(*byte));
        let value = match (start, end) {
            (Some(start), Some(end)) => &bytes[start..=end],
            _ => &[],
        };

        // 2. If value is not a header value, return a promise rejected with a TypeError.
        // https://fetch.spec.whatwg.org/#header-value
        // A header value is a byte sequence that matches the following conditions:
        // - Has no leading or trailing HTTP tab or space bytes.
        // - Contains no 0x00 (NUL) or HTTP newline bytes.
        if value.iter().any(|byte| matches!(byte, 0x00 | b'\n' | b'\r')) {
            return Ok(create_rejected_promise_from_exception(
                cx.vm(),
                self.realm,
                Exception::TypeError(Utf16String::from_utf8("The value is not a header value")),
            ));
        }

        Ok(self.request(cx.vm(), NavigationPreloadOperation::SetHeaderValue, value))
    }

    // https://w3c.github.io/ServiceWorker/#dom-navigationpreloadmanager-getstate
    pub(crate) fn get_state(&self, cx: &mut CallContext) -> WebIdlResult<WebIdlPromise> {
        Ok(self.request(cx.vm(), NavigationPreloadOperation::GetState, &[]))
    }
}

/// What the UI process answered a request of a NavigationPreloadManager with: the registration's navigation preload
/// state, or nothing when the registration has no active worker.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct NavigationPreloadAnswer {
    promise: WebIdlPromise,
    #[gc(untraced)]
    is_state_request: bool,
    #[gc(untraced)]
    state: Option<(bool, Vec<u8>)>,
}

static NAVIGATION_PRELOAD_ANSWER_CLASS: EmbedderClass<NavigationPreloadAnswer> =
    EmbedderClass::new("NavigationPreloadAnswer");

impl EmbedderValue for NavigationPreloadAnswer {
    const CLASS: &'static EmbedderClass<Self> = &NAVIGATION_PRELOAD_ANSWER_CLASS;
}

pub(crate) fn navigation_preload_settled(vm: &Vm, request_id: u64, state: Option<(bool, Vec<u8>)>) {
    let (request, is_state_request) = if let Some((request, _)) =
        take_pending_request(request_id, ServiceWorkerRequestKind::NavigationPreloadUpdate)
    {
        (request, false)
    } else if let Some((request, _)) =
        take_pending_request(request_id, ServiceWorkerRequestKind::NavigationPreloadState)
    {
        (request, true)
    } else {
        return;
    };
    let promise = request.promise;
    let answer = NAVIGATION_PRELOAD_ANSWER_CLASS.allocate(vm, |_| NavigationPreloadAnswer {
        promise,
        is_state_request,
        state,
    });

    // NB: The UI process gives no state for a registration without an active worker, whose promise is rejected with an
    //     "InvalidStateError" DOMException. A request for the state is resolved with it, and any other with undefined,
    //     in a task on the promise's relevant settings object's responsible event loop.
    queue_a_global_task(
        vm,
        promise_realm(promise).global_object(),
        TaskSource::DOMManipulation,
        answer,
        |vm, answer: Gc<EmbedderCell<NavigationPreloadAnswer>>| {
            let realm = promise_realm(answer.promise);
            with_temporary_execution_context_and_callbacks(realm, || {
                let Some((enabled, header_value)) = answer.state.clone() else {
                    reject_promise_with_exception(
                        vm,
                        answer.promise,
                        Exception::Dom(
                            "InvalidStateError",
                            Utf16String::from_utf8("The registration has no active worker"),
                        ),
                    );
                    return;
                };
                if !answer.is_state_request {
                    resolve_promise(vm, answer.promise, Value::UNDEFINED);
                    return;
                }

                // NB: The state converts to a JS value as the dictionary NavigationPreloadState does.
                let cx = CallContext::new(vm, realm);
                let state = create_dictionary_object(&cx);
                let members = [
                    ("enabled", Value::from_bool(enabled)),
                    ("headerValue", Utf16String::from_latin1(&header_value).to_js(&cx).must()),
                ];
                for (name, value) in members {
                    set_dictionary_member(&cx, state, &PropertyKey::from_utf8(name), value).must();
                }
                resolve_promise(vm, answer.promise, Value::from_object(state));
            });
        },
    );
}
