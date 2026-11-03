/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! ServiceWorkerContainer, https://w3c.github.io/ServiceWorker/#serviceworkercontainer-interface

use core::cell::{Cell, RefCell};
use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{
    Array, CellRoot, EmbedderCell, EmbedderClass, EmbedderValue, Gc, GcRefCell, GcWeak, IntegrityLevel, MarkedVec,
    Must, Object, RawNativeFunctionResult, Realm, Trace, Value, Vm,
};
use liburl_rust::FfiOrigin;
use liburl_rust::origin::Origin;
use liburl_rust::url::{BasicParseOptions, ExcludeFragment, Url, basic_parse};

use crate::bindings::conversions::{TrustedScriptURLOrUSVString, UsvString};
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::global_object::window_from_global_object;
use crate::bindings::interface::{Ref, Wrappable, allocate, with_temporary_execution_context_and_callbacks, wrap};
use crate::bindings::promise::{
    TaskSource, WebIdlPromise, create_promise, create_rejected_promise_from_exception, promise_realm,
    queue_a_global_task, queue_a_task, reject_promise_with_exception, resolve_promise,
};
use crate::bindings::realm_data::realm_data;
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::domurl::url_input;
use crate::html::worker::WorkerType;
use crate::service_worker::service_worker::{ServiceWorker, ServiceWorkerState, get_the_service_worker_object};
use crate::service_worker::service_worker_registration::{
    ServiceWorkerUpdateViaCache, get_the_service_worker_registration_object,
};
use crate::service_worker::{ServiceWorkerDescriptor, ServiceWorkerRegistrationSnapshot};

unsafe extern "C" {
    /// Get Trusted Type compliant string with TrustedScriptURL, the realm's global object, the input, "ServiceWorkerContainer
    /// register", and "script". The input is the TrustedScriptURL that the wrapper wraps, if there is one, and otherwise
    /// the raw AK::Utf16String, which the call takes. The result is a string, or what the algorithm throws.
    fn web_service_worker_get_trusted_type_compliant_script_url(
        realm: *mut c_void,
        trusted_script_url: *mut c_void,
        string: usize,
    ) -> RawNativeFunctionResult;

    /// Calls the function with the API base URL and the origin of the realm's settings object, which it lends for the
    /// call.
    fn web_service_worker_service_worker_client(
        realm: *mut c_void,
        ctx: *mut c_void,
        on_client: unsafe extern "C" fn(*mut c_void, ak::Span<'_, u8>, *const FfiOrigin),
    );

    /// Calls the function with the active service worker of the realm's settings object, if it has one, which it lends
    /// for the call.
    fn web_service_worker_active_service_worker(
        realm: *mut c_void,
        ctx: *mut c_void,
        on_service_worker: unsafe extern "C" fn(*mut c_void, u64, ak::Span<'_, u8>, u8),
    );

    /// Has the UI process schedule a job of the type, with the realm's settings object as its client, which settles as
    /// web_rust_service_worker_job_settled() says.
    fn web_service_worker_schedule_job(
        realm: *mut c_void,
        request_id: u64,
        job_type: u8,
        scope_url: ak::Span<'_, u8>,
        script_url: ak::Span<'_, u8>,
        worker_type: u8,
        update_via_cache: u8,
    );

    /// Has the UI process resolve the ready promise of the realm's settings object, which
    /// web_rust_service_worker_ready() reports.
    fn web_service_worker_request_ready(realm: *mut c_void, request_id: u64);

    /// Has the UI process look up the registration that matches the client URL, or every registration if the URL is
    /// empty, for the realm's settings object, which web_rust_service_worker_registration_lookup_finished() reports.
    fn web_service_worker_request_registrations(realm: *mut c_void, request_id: u64, client_url: ak::Span<'_, u8>);
}

/// The dictionary RegistrationOptions of ServiceWorker/ServiceWorkerContainer.idl.
pub(crate) struct RegistrationOptions {
    pub(crate) scope: Option<UsvString>,
    pub(crate) r#type: WorkerType,
    pub(crate) update_via_cache: ServiceWorkerUpdateViaCache,
}

// https://w3c.github.io/ServiceWorker/#serviceworkercontainer-interface
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ServiceWorkerContainer {
    event_target: CppEventTarget,
    realm: Gc<Realm>,

    // A ServiceWorkerContainer object has an associated ready promise (a promise or null). It is initially null.
    // NB: A container that another wrapper world reaches has a ready promise for each world's realm.
    ready_promises: GcRefCell<Vec<(Gc<Realm>, WebIdlPromise)>>,
}

impl Wrappable for ServiceWorkerContainer {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.realm)
    }

    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event_target.as_cpp_parent()
    }
}

impl GetTheParent for ServiceWorkerContainer {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

/// What the methods of a container read of its service worker client.
struct ServiceWorkerClient {
    api_base_url: Url,
    origin: Origin,
}

// https://w3c.github.io/ServiceWorker/#serviceworkercontainer-service-worker-client
fn service_worker_client(realm: Gc<Realm>) -> ServiceWorkerClient {
    unsafe extern "C" fn on_client(ctx: *mut c_void, api_base_url: ak::Span<'_, u8>, origin: *const FfiOrigin) {
        let api_base_url = core::str::from_utf8(api_base_url.as_slice()).expect("LibWeb lends a serialized URL");
        // SAFETY: ctx is the client below, and LibWeb lends the origin for the call.
        unsafe {
            *ctx.cast::<Option<ServiceWorkerClient>>() = Some(ServiceWorkerClient {
                api_base_url: basic_parse(api_base_url, BasicParseOptions::new())
                    .expect("LibWeb lends a serialized URL"),
                origin: (*origin).to_origin(),
            });
        }
    }

    let mut client: Option<ServiceWorkerClient> = None;
    // SAFETY: The realm is live, and the client outlives the call.
    unsafe { web_service_worker_service_worker_client(realm.as_ptr().cast(), (&raw mut client).cast(), on_client) };
    client.expect("LibWeb lends the service worker client")
}

// https://url.spec.whatwg.org/#concept-url-parser
fn parse_with_base(input: &UsvString, base: &Url) -> Option<Url> {
    basic_parse(url_input(input.units()), BasicParseOptions::new().base_url(base))
}

fn has_scheme_http_or_https(url: &Url) -> bool {
    matches!(url.scheme(), "http" | "https")
}

fn path_contains_escaped_slash_or_backslash(url: &Url) -> bool {
    url.path_segments().any(|segment| {
        let segment = segment.to_ascii_lowercase();
        segment.contains("%2f") || segment.contains("%5c")
    })
}

/// The kinds of request that a container sends the UI process, which say how their promises settle.
#[derive(Clone, Copy, PartialEq, Eq)]
enum ServiceWorkerRequestKind {
    Job,
    GetRegistration,
    GetRegistrations,
    Ready,
}

// SAFETY: A kind of request holds no cells.
unsafe impl Trace for ServiceWorkerRequestKind {
    fn trace(&self, _: &mut libjs_rust::api::Visitor) {}
}

/// The promise of a request to the UI process, which settles once its answer arrives.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct PendingRequest {
    promise: WebIdlPromise,
    kind: ServiceWorkerRequestKind,
    #[gc(untraced)]
    answered: Cell<bool>,
}

static PENDING_REQUEST_CLASS: EmbedderClass<PendingRequest> = EmbedderClass::new("ServiceWorkerPendingRequest");

impl EmbedderValue for PendingRequest {
    const CLASS: &'static EmbedderClass<Self> = &PENDING_REQUEST_CLASS;
}

pub(crate) type PendingRequestRef = Gc<EmbedderCell<PendingRequest>>;
type PendingRequestRoot = CellRoot<EmbedderCell<PendingRequest>>;
type PendingRequestWeak = GcWeak<EmbedderCell<PendingRequest>>;

thread_local! {
    static NEXT_REQUEST_ID: Cell<u64> = const { Cell::new(0) };
    // NB: The realm of a request's promise keeps the request alive, so an answer that never comes, such as one for a
    //     document the UI process has discarded, does not keep the realm alive. A request whose realm has been
    //     collected leaves its entry empty.
    static PENDING_REQUESTS: RefCell<Vec<(u64, PendingRequestWeak)>> = const { RefCell::new(Vec::new()) };
}

/// Lets go of the requests of an agent that is going away, whose weak references would outlive its heap.
pub(crate) fn forget_pending_requests_of_agent() {
    PENDING_REQUESTS.with_borrow_mut(Vec::clear);
}

fn add_pending_request(vm: &Vm, promise: WebIdlPromise, kind: ServiceWorkerRequestKind) -> u64 {
    let request_id = NEXT_REQUEST_ID.with(|next| {
        next.set(next.get() + 1);
        next.get()
    });
    let request = PENDING_REQUEST_CLASS.allocate(vm, |_| PendingRequest {
        promise,
        kind,
        answered: Cell::new(false),
    });

    // NB: The realm lets go of the requests that have been answered once it makes another.
    {
        let data = realm_data(vm, promise_realm(promise));
        let mut realm_requests = data.service_worker_pending_requests.borrow_mut(vm);
        realm_requests.retain(|request| !request.answered.get());
        realm_requests.push(request);
    }

    PENDING_REQUESTS.with_borrow_mut(|requests| {
        requests.retain(|(_, request)| request.get().is_some());
        requests.push((request_id, GcWeak::new(vm.heap(), request)));
    });
    request_id
}

/// The request's promise, and a root that keeps it alive while its caller holds it.
fn take_pending_request(
    request_id: u64,
    kind: ServiceWorkerRequestKind,
) -> Option<(PendingRequestRef, PendingRequestRoot)> {
    let request = PENDING_REQUESTS.with_borrow_mut(|requests| {
        let index = requests
            .iter()
            .position(|(id, request)| *id == request_id && request.get().is_some_and(|request| request.kind == kind))?;
        requests.remove(index).1.get()
    })?;
    let root = CellRoot::new(request);
    request.answered.set(true);
    Some((request, root))
}

/// How the UI process says that a job settled: with a registration, a boolean, or an exception.
pub(crate) enum ServiceWorkerJobResult {
    Registration(ServiceWorkerRegistrationSnapshot),
    Boolean(bool),
    TypeError,
    SecurityError,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct JobSettlement {
    promise: WebIdlPromise,
    #[gc(untraced)]
    result: ServiceWorkerJobResult,
}

static JOB_SETTLEMENT_CLASS: EmbedderClass<JobSettlement> = EmbedderClass::new("ServiceWorkerJobSettlement");

impl EmbedderValue for JobSettlement {
    const CLASS: &'static EmbedderClass<Self> = &JOB_SETTLEMENT_CLASS;
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct RegistrationLookup {
    promise: WebIdlPromise,
    kind: ServiceWorkerRequestKind,
    #[gc(untraced)]
    registrations: Vec<ServiceWorkerRegistrationSnapshot>,
}

static REGISTRATION_LOOKUP_CLASS: EmbedderClass<RegistrationLookup> =
    EmbedderClass::new("ServiceWorkerRegistrationLookup");

impl EmbedderValue for RegistrationLookup {
    const CLASS: &'static EmbedderClass<Self> = &REGISTRATION_LOOKUP_CLASS;
}

impl ServiceWorkerContainer {
    pub(crate) fn create(vm: &Vm, realm: Gc<Realm>) -> Ref<Self> {
        let container = allocate(vm, |_| Self {
            event_target: CppEventTarget::new(),
            realm,
            ready_promises: GcRefCell::new(Vec::new()),
        });
        container.event_target.set_owner(container);
        container
    }

    // https://w3c.github.io/ServiceWorker/#dom-serviceworkercontainer-controller
    pub(crate) fn controller(&self, cx: &mut CallContext) -> WebIdlResult<Option<Ref<ServiceWorker>>> {
        unsafe extern "C" fn on_service_worker(ctx: *mut c_void, id: u64, script_url: ak::Span<'_, u8>, state: u8) {
            // SAFETY: ctx is the service worker below, and LibWeb lends the script URL for the call.
            unsafe {
                *ctx.cast::<Option<ServiceWorkerDescriptor>>() = Some(ServiceWorkerDescriptor {
                    id,
                    script_url: String::from_utf8_lossy(script_url.as_slice()).into_owned(),
                    state: ServiceWorkerState::from_u8(state).expect("LibWeb passes a ServiceWorkerState"),
                });
            }
        }

        // 1. Let client be this's service worker client.
        // 2. If client's active service worker is null, then return null.
        let mut active_service_worker: Option<ServiceWorkerDescriptor> = None;
        // SAFETY: The realm is live, and the service worker outlives the call.
        unsafe {
            web_service_worker_active_service_worker(
                self.realm.as_ptr().cast(),
                (&raw mut active_service_worker).cast(),
                on_service_worker,
            );
        }
        let Some(active_service_worker) = active_service_worker else {
            return Ok(None);
        };

        // 3. Return the result of getting the service worker object that represents client's active service worker in
        //    this's relevant settings object.
        Ok(Some(get_the_service_worker_object(
            cx.vm(),
            self.realm,
            &active_service_worker,
        )))
    }

    // https://w3c.github.io/ServiceWorker/#dom-serviceworkercontainer-ready
    pub(crate) fn ready(&self, cx: &mut CallContext) -> WebIdlResult<WebIdlPromise> {
        let vm = cx.vm();
        let realm = cx.realm();

        // 1. If this's ready promise is null, then set this's ready promise to a new promise.
        // 2. Let readyPromise be this's ready promise.
        let existing_promise = self
            .ready_promises
            .borrow()
            .iter()
            .find(|(promise_realm, _)| *promise_realm == realm)
            .map(|(_, promise)| *promise);
        if let Some(ready_promise) = existing_promise {
            // 4. Return readyPromise.
            return Ok(ready_promise);
        }
        let ready_promise = create_promise(vm, realm);
        self.ready_promises.borrow_mut(vm).push((realm, ready_promise));

        // 3. If readyPromise is pending, run the following substeps in parallel:
        // NB: The UI process runs the substeps, and keeps the promise until it can resolve it. A promise that is no
        //     longer new is pending only while the UI process holds it, so this asks only for a new one.
        let request_id = add_pending_request(vm, ready_promise, ServiceWorkerRequestKind::Ready);
        // SAFETY: The realm is live.
        unsafe { web_service_worker_request_ready(self.realm.as_ptr().cast(), request_id) };

        // 4. Return readyPromise.
        // NOTE: The returned ready promise will never reject. If it does not resolve in this algorithm, it will
        //       eventually resolve when a matching service worker registration is registered and its active worker is
        //       set. (See the relevant Activate algorithm step.)
        Ok(ready_promise)
    }

    // https://w3c.github.io/ServiceWorker/#dom-serviceworkercontainer-register
    pub(crate) fn register(
        &self,
        cx: &mut CallContext,
        script_url: TrustedScriptURLOrUSVString,
        options: RegistrationOptions,
    ) -> WebIdlResult<WebIdlPromise> {
        let vm = cx.vm();

        // NOTE: The register(scriptURL, options) method creates or updates a service worker registration for the given
        //       scope url. If successful, a service worker registration ties the provided scriptURL to a scope url,
        //       which is subsequently used for navigation matching.

        // 1. Let p be a promise.
        let promise = create_promise(vm, self.realm);

        // 2. Set scriptURL to the result of invoking Get Trusted Type compliant string with TrustedScriptURL, this's
        //    relevant global object, scriptURL, "ServiceWorkerContainer register", and "script".
        let script_url = get_trusted_type_compliant_script_url(vm, self.realm, script_url)?;

        // 3. Let client be this's service worker client.
        let client = service_worker_client(self.realm);

        // 4. Let scriptURL be the result of parsing scriptURL with this's relevant settings object's API base URL.
        let script_url = parse_with_base(&script_url, &client.api_base_url);

        // 5. Let scopeURL be null.
        // 6. If options["scope"] exists, set scopeURL to the result of parsing options["scope"] with this's relevant
        //    settings object's API base URL.
        let scope_url = options.scope.map(|scope| parse_with_base(&scope, &client.api_base_url));

        // 7. Invoke Start Register with scopeURL, scriptURL, p, client, client's creation URL, options["type"], and
        //    options["updateViaCache"].
        start_register(
            vm,
            self.realm,
            scope_url,
            script_url,
            promise,
            options.r#type,
            options.update_via_cache,
        );

        // 8. Return p.
        Ok(promise)
    }

    // https://w3c.github.io/ServiceWorker/#dom-serviceworkercontainer-getregistration
    pub(crate) fn get_registration(&self, cx: &mut CallContext, client_url: UsvString) -> WebIdlResult<WebIdlPromise> {
        let vm = cx.vm();

        // 1. Let client be this's service worker client.
        let client = service_worker_client(self.realm);

        // 2. Let storage key be the result of running obtain a storage key given client.
        // NB: The UI process obtains the storage key, with which it runs step 8.1.

        // 3. Let clientURL be the result of parsing clientURL with this's relevant settings object's API base URL.
        // 4. If clientURL is failure, return a promise rejected with a TypeError.
        let Some(mut client_url) = parse_with_base(&client_url, &client.api_base_url) else {
            return Ok(create_rejected_promise_from_exception(
                vm,
                self.realm,
                Exception::type_error("clientURL is not a valid URL"),
            ));
        };

        // 5. Set clientURL's fragment to null.
        client_url.set_fragment(None);

        // 6. If the origin of clientURL is not client's origin, return a promise rejected with a "SecurityError"
        //    DOMException.
        if !client_url.origin().is_same_origin(&client.origin) {
            return Ok(create_rejected_promise_from_exception(
                vm,
                self.realm,
                Exception::Dom(
                    "SecurityError",
                    Utf16String::from_utf8("clientURL is not the same origin as the client's origin"),
                ),
            ));
        }

        // 7. Let promise be a new promise.
        let promise = create_promise(vm, self.realm);

        // 8. Run the following substeps in parallel:
        //    1. Let registration be the result of running Match Service Worker Registration given storage key and
        //       clientURL.
        //    2. Queue a task on promise's relevant settings object's responsible event loop, using the DOM manipulation
        //       task source, to run the following steps:
        // NB: The UI process matches the registration, and service_worker_registration_lookup_finished() queues the
        //     task.
        let request_id = add_pending_request(vm, promise, ServiceWorkerRequestKind::GetRegistration);
        let client_url = client_url.serialize(ExcludeFragment::No);
        // SAFETY: The realm is live, and the URL is lent for the call.
        unsafe {
            web_service_worker_request_registrations(
                self.realm.as_ptr().cast(),
                request_id,
                ak::Span::new(client_url.as_bytes()),
            );
        }

        // 9. Return promise.
        Ok(promise)
    }

    // https://w3c.github.io/ServiceWorker/#dom-serviceworkercontainer-getregistrations
    pub(crate) fn get_registrations(&self, cx: &mut CallContext) -> WebIdlResult<WebIdlPromise> {
        let vm = cx.vm();

        // 1. Let client be this's service worker client.
        // 2. Let client storage key be the result of running obtain a storage key given client.
        // NB: The UI process obtains the storage key, with which it runs step 4.

        // 3. Let promise be a new promise.
        let promise = create_promise(vm, self.realm);

        // 4. Run the following steps in parallel:
        // NB: The UI process lists the registrations, and service_worker_registration_lookup_finished() queues the task
        //     of step 4.3.
        let request_id = add_pending_request(vm, promise, ServiceWorkerRequestKind::GetRegistrations);
        // SAFETY: The realm is live.
        unsafe {
            web_service_worker_request_registrations(self.realm.as_ptr().cast(), request_id, ak::Span::default());
        };

        // 5. Return promise.
        Ok(promise)
    }
}

fn get_trusted_type_compliant_script_url(
    vm: &Vm,
    realm: Gc<Realm>,
    script_url: TrustedScriptURLOrUSVString,
) -> WebIdlResult<UsvString> {
    let (trusted_script_url, string) = match script_url {
        TrustedScriptURLOrUSVString::TrustedScriptURL(trusted_script_url) => (
            trusted_script_url.wrapper().as_ptr().cast(),
            Utf16String::from_ascii(b"").into_raw(),
        ),
        TrustedScriptURLOrUSVString::USVString(string) => {
            (core::ptr::null_mut(), string.into_utf16_string().into_raw())
        }
    };
    // SAFETY: The realm and the wrapper are live, and the call takes the string.
    let result = unsafe {
        web_service_worker_get_trusted_type_compliant_script_url(realm.as_ptr().cast(), trusted_script_url, string)
    }
    .into_completion()?;
    let cx = CallContext::new(vm, realm);
    Ok(crate::bindings::conversions::to_usv_string(cx.vm(), result)?)
}

// https://w3c.github.io/ServiceWorker/#start-register-algorithm
fn start_register(
    vm: &Vm,
    realm: Gc<Realm>,
    scope_url: Option<Option<Url>>,
    script_url: Option<Url>,
    promise: WebIdlPromise,
    worker_type: WorkerType,
    update_via_cache: ServiceWorkerUpdateViaCache,
) {
    let reject = |message: &str| reject_promise_with_exception(vm, promise, Exception::type_error(message));

    // 1. If scriptURL is failure, reject promise with a TypeError and abort these steps.
    let Some(mut script_url) = script_url else {
        reject("scriptURL is not a valid URL");
        return;
    };

    // 2. Set scriptURL's fragment to null.
    // NOTE: The user agent does not store the fragment of the script's url. This means that the fragment does not have
    //       an effect on identifying service workers.
    script_url.set_fragment(None);

    // 3. If scriptURL's scheme is not one of "http" and "https", reject promise with a TypeError and abort these steps.
    if !has_scheme_http_or_https(&script_url) {
        reject("scriptURL must have a scheme of 'http' or 'https'");
        return;
    }

    // 4. If any of the strings in scriptURL's path contains either ASCII case-insensitive "%2f" or ASCII
    //    case-insensitive "%5c", reject promise with a TypeError and abort these steps.
    if path_contains_escaped_slash_or_backslash(&script_url) {
        reject("scriptURL path must not contain '%2f' or '%5c'");
        return;
    }

    // 5. If scopeURL is null, set scopeURL to the result of parsing the string "./" with scriptURL.
    // NOTE: The scope url for the registration is set to the location of the service worker script by default.
    let scope_url = scope_url.unwrap_or_else(|| basic_parse("./", BasicParseOptions::new().base_url(&script_url)));

    // 6. If scopeURL is failure, reject promise with a TypeError and abort these steps.
    let Some(mut scope_url) = scope_url else {
        reject("scopeURL is not a valid URL");
        return;
    };

    // 7. Set scopeURL's fragment to null.
    // NOTE: The user agent does not store the fragment of the scope url. This means that the fragment does not have an
    //       effect on identifying service worker registrations.
    scope_url.set_fragment(None);

    // 8. If scopeURL's scheme is not one of "http" and "https", reject promise with a TypeError and abort these steps.
    if !has_scheme_http_or_https(&scope_url) {
        reject("scopeURL must have a scheme of 'http' or 'https'");
        return;
    }

    // 9. If any of the strings in scopeURL's path contains either ASCII case-insensitive "%2f" or ASCII
    //    case-insensitive "%5c", reject promise with a TypeError and abort these steps.
    if path_contains_escaped_slash_or_backslash(&scope_url) {
        reject("scopeURL path must not contain '%2f' or '%5c'");
        return;
    }

    // 10. Let storage key be the result of running obtain a storage key given client.
    // 11. Let job be the result of running Create Job with register, storage key, scopeURL, scriptURL, promise, and
    //     client.
    // 12. Set job's worker type to workerType.
    // 13. Set job's update via cache mode to updateViaCache.
    // 14. Set job's referrer to referrer.
    // 15. Invoke Schedule Job with job.
    // NB: The UI process creates and schedules the job, with the storage key and the referrer it knows the client by.
    //     The job promise settles once this process learns how the job settled.
    schedule_job(
        vm,
        realm,
        promise,
        ServiceWorkerJobType::Register,
        scope_url.serialize(ExcludeFragment::No),
        Some(script_url.serialize(ExcludeFragment::No)),
        worker_type,
        update_via_cache,
    );
}

/// In the order of Web::ServiceWorker::ServiceWorkerJobType, which C++ passes as its underlying value,
/// https://w3c.github.io/ServiceWorker/#dfn-job-type
#[derive(Clone, Copy)]
#[repr(u8)]
pub(crate) enum ServiceWorkerJobType {
    Register = 0,
    Unregister = 2,
}

/// Has the UI process create and schedule a job, whose promise settles once this process learns how the job settled.
#[allow(clippy::too_many_arguments, reason = "a job has this many parts")]
pub(crate) fn schedule_job(
    vm: &Vm,
    realm: Gc<Realm>,
    promise: WebIdlPromise,
    job_type: ServiceWorkerJobType,
    scope_url: &str,
    script_url: Option<&str>,
    worker_type: WorkerType,
    update_via_cache: ServiceWorkerUpdateViaCache,
) {
    // AD-HOC: The job of a client whose document is not fully active is rejected with an "InvalidStateError"
    //         DOMException, matching other engines.
    if let Some(window) = window_from_global_object(realm.global_object())
        && !window.associated_document().is_fully_active()
    {
        reject_promise_with_exception(
            vm,
            promise,
            Exception::Dom(
                "InvalidStateError",
                Utf16String::from_utf8("The service worker client's document is not fully active"),
            ),
        );
        return;
    }

    let request_id = add_pending_request(vm, promise, ServiceWorkerRequestKind::Job);
    // SAFETY: The realm is live, and the URLs are lent for the call.
    unsafe {
        web_service_worker_schedule_job(
            realm.as_ptr().cast(),
            request_id,
            job_type as u8,
            ak::Span::new(scope_url.as_bytes()),
            ak::Span::new(script_url.unwrap_or_default().as_bytes()),
            worker_type as u8,
            update_via_cache as u8,
        );
    }
}

/// The job of the request settled with the result, https://w3c.github.io/ServiceWorker/#resolve-job-promise-algorithm
/// and https://w3c.github.io/ServiceWorker/#reject-job-promise-algorithm
pub(crate) fn service_worker_job_settled(vm: &Vm, request_id: u64, result: ServiceWorkerJobResult) {
    let Some((request, _root)) = take_pending_request(request_id, ServiceWorkerRequestKind::Job) else {
        return;
    };
    let promise = request.promise;
    let settlement = JOB_SETTLEMENT_CLASS.allocate(vm, |_| JobSettlement { promise, result });

    // Queue a task, on job's client's responsible event loop using the DOM manipulation task source, to run the
    // following substeps:
    queue_a_task(
        vm,
        promise_realm(promise),
        TaskSource::DOMManipulation,
        settlement,
        |vm, settlement| {
            let realm = promise_realm(settlement.promise);
            with_temporary_execution_context_and_callbacks(realm, || match &settlement.result {
                ServiceWorkerJobResult::Registration(registration) => {
                    // 1. Let convertedValue be null.
                    // 2. If job's job type is either register or update, set convertedValue to the result of getting
                    //    the service worker registration object that represents value in job's client.
                    let converted_value = get_the_service_worker_registration_object(vm, realm, registration);

                    // 4. Resolve job's job promise with convertedValue.
                    let cx = CallContext::new(vm, realm);
                    resolve_promise(vm, settlement.promise, Value::from_object(wrap(&cx, converted_value)));
                }
                ServiceWorkerJobResult::Boolean(value) => {
                    // 3. Else, set convertedValue to value, in job's client's Realm.
                    // 4. Resolve job's job promise with convertedValue.
                    resolve_promise(vm, settlement.promise, Value::from_bool(*value));
                }
                // To reject job's job promise with a new exception with errorData, in job's client's Realm.
                ServiceWorkerJobResult::TypeError => reject_promise_with_exception(
                    vm,
                    settlement.promise,
                    Exception::type_error("Service worker registration failed"),
                ),
                ServiceWorkerJobResult::SecurityError => reject_promise_with_exception(
                    vm,
                    settlement.promise,
                    Exception::Dom(
                        "SecurityError",
                        Utf16String::from_utf8("Service worker registration is not allowed"),
                    ),
                ),
            });
        },
    );
}

/// The UI process found the registrations of a request of getRegistration() or getRegistrations().
pub(crate) fn service_worker_registration_lookup_finished(
    vm: &Vm,
    request_id: u64,
    registrations: Vec<ServiceWorkerRegistrationSnapshot>,
) {
    let request = take_pending_request(request_id, ServiceWorkerRequestKind::GetRegistration)
        .or_else(|| take_pending_request(request_id, ServiceWorkerRequestKind::GetRegistrations));
    let Some((request, _root)) = request else {
        return;
    };
    let (promise, kind) = (request.promise, request.kind);
    let lookup = REGISTRATION_LOOKUP_CLASS.allocate(vm, |_| RegistrationLookup {
        promise,
        kind,
        registrations,
    });

    queue_a_global_task(
        vm,
        promise_realm(promise).global_object(),
        TaskSource::DOMManipulation,
        lookup,
        |vm, lookup| {
            let realm = promise_realm(lookup.promise);
            with_temporary_execution_context_and_callbacks(realm, || resolve_registration_lookup(vm, realm, &lookup));
        },
    );
}

fn resolve_registration_lookup(vm: &Vm, realm: Gc<Realm>, lookup: &RegistrationLookup) {
    let cx = CallContext::new(vm, realm);

    // https://w3c.github.io/ServiceWorker/#dom-serviceworkercontainer-getregistration
    if lookup.kind == ServiceWorkerRequestKind::GetRegistration {
        // 1. If registration is null, resolve promise with undefined and abort these steps.
        let Some(registration) = lookup.registrations.first() else {
            resolve_promise(vm, lookup.promise, Value::UNDEFINED);
            return;
        };

        // 2. Resolve promise with the result of getting the service worker registration object that represents
        //    registration in promise's relevant settings object.
        let registration_object = get_the_service_worker_registration_object(vm, realm, registration);
        resolve_promise(vm, lookup.promise, Value::from_object(wrap(&cx, registration_object)));
        return;
    }

    // https://w3c.github.io/ServiceWorker/#dom-serviceworkercontainer-getregistrations
    // 1. Let registrationObjects be a new list.
    let registration_objects = MarkedVec::new(vm);

    // 2. For each registration of registrations:
    for registration in &lookup.registrations {
        // 1. Let registrationObj be the result of getting the service worker registration object that
        //    represents registration in promise's relevant settings object.
        let registration_object = get_the_service_worker_registration_object(vm, realm, registration);

        // 2. Append registrationObj to registrationObjects.
        registration_objects.push(Value::from_object(wrap(&cx, registration_object)));
    }

    // 3. Resolve promise with a new frozen array of registrationObjects in promise's relevant Realm.
    let array = Array::create_from_list(vm, realm, &registration_objects);
    array.set_integrity_level(vm, IntegrityLevel::Frozen).must();
    resolve_promise(vm, lookup.promise, Value::from_object(array.upcast::<Object>()));
}

// https://w3c.github.io/ServiceWorker/#activation-algorithm
pub(crate) fn service_worker_ready(vm: &Vm, request_id: u64, registration: ServiceWorkerRegistrationSnapshot) {
    let Some((request, _root)) = take_pending_request(request_id, ServiceWorkerRequestKind::Ready) else {
        return;
    };
    let promise = request.promise;
    let lookup = REGISTRATION_LOOKUP_CLASS.allocate(vm, |_| RegistrationLookup {
        promise,
        kind: ServiceWorkerRequestKind::Ready,
        registrations: vec![registration],
    });

    // 7. For each client of matchedClients, queue a task on client's responsible event loop, using the DOM manipulation
    //    task source, to run the following substeps:
    queue_a_global_task(
        vm,
        promise_realm(promise).global_object(),
        TaskSource::DOMManipulation,
        lookup,
        |vm, lookup| {
            let realm = promise_realm(lookup.promise);
            with_temporary_execution_context_and_callbacks(realm, || {
                // 1. Let readyPromise be client's global object's ServiceWorkerContainer object's ready promise.
                // 2. If readyPromise is null, then continue.
                // 3. If readyPromise is pending, resolve readyPromise with the result of getting the service worker
                //    registration object that represents registration in readyPromise's relevant settings object.
                let cx = CallContext::new(vm, realm);
                let registration_object =
                    get_the_service_worker_registration_object(vm, realm, &lookup.registrations[0]);
                resolve_promise(vm, lookup.promise, Value::from_object(wrap(&cx, registration_object)));
            });
        },
    );
}
