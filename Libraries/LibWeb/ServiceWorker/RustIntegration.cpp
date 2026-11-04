/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibURL/Parser.h>
#include <LibURL/RustIntegration.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Bindings/RustSerialization.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/DOMStringList.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Navigator.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/SharedWorkerGlobalScope.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/HTML/WorkerNavigator.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/ServiceWorker/Clients.h>
#include <LibWeb/ServiceWorker/ServiceWorker.h>
#include <LibWeb/ServiceWorker/ServiceWorkerContainer.h>
#include <LibWeb/ServiceWorker/ServiceWorkerGlobalScope.h>
#include <LibWeb/ServiceWorker/ServiceWorkerRegistration.h>
#include <LibWeb/TrustedTypes/RequireTrustedTypesForDirective.h>
#include <LibWeb/TrustedTypes/TrustedScriptURL.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>

// What the Rust implementation of the service worker interfaces asks of LibWeb's C++ code, declared on the Rust side
// in Libraries/LibWeb/Bindings/Rust/src/service_worker.

namespace Web::ServiceWorker {

// Libraries/LibWeb/Bindings/Rust/src/service_worker/exports.rs
struct FfiServiceWorkerDescriptor {
    bool exists { false };
    u64 id { 0 };
    ReadonlyBytes script_url;
    u8 state { 0 };
};

struct FfiServiceWorkerClientSnapshot {
    ReadonlyBytes id;
    ReadonlyBytes url;
    u8 type { 0 };
    u8 frame_type { 0 };
    u8 visibility_state { 0 };
    bool focused { false };
    ReadonlySpan<ReadonlyBytes> ancestor_origins;
};

struct FfiServiceWorkerRegistrationSnapshot {
    u64 id { 0 };
    ReadonlyBytes scope_url;
    u8 update_via_cache { 0 };
    FfiServiceWorkerDescriptor installing_worker;
    FfiServiceWorkerDescriptor waiting_worker;
    FfiServiceWorkerDescriptor active_worker;
};

extern "C" {

JSCompletion web_service_worker_get_trusted_type_compliant_script_url(void* realm, void* trusted_script_url, FlatPtr string);
void web_service_worker_service_worker_client(void* realm, void* ctx, void (*on_client)(void*, ReadonlyBytes, URL::FFI::FfiOrigin const*));
void web_service_worker_active_service_worker(void* realm, void* ctx, void (*on_service_worker)(void*, u64, ReadonlyBytes, u8));
void web_service_worker_schedule_job(void* realm, u64 request_id, u8 job_type, ReadonlyBytes scope_url, ReadonlyBytes script_url, u8 worker_type, u8 update_via_cache);
void web_service_worker_request_registrations(void* realm, u64 request_id, ReadonlyBytes client_url);
void web_service_worker_request_ready(void* realm, u64 request_id);
void web_service_worker_post_message_to_service_worker(void* realm, u64 service_worker_id, HTML::SerializedTransferRecord* record);
void web_service_worker_post_message_to_client(void* realm, ReadonlyBytes client_id, HTML::SerializedTransferRecord* record);
void* web_service_worker_container_of_realm(void* realm);
void web_service_worker_request_navigation_preload(void* realm, u64 request_id, u64 registration_id, u8 operation, ReadonlyBytes header_value);
void web_service_worker_request_clients(void* realm, u64 request_id, ReadonlyBytes client_id, bool is_get, bool include_uncontrolled, u8 type);
bool web_service_worker_claim(void* realm, u64 request_id);

// Defined on the Rust side.
void* web_rust_service_worker_create_container(JSVM*, void* realm);
void web_rust_service_worker_job_settled(JSVM*, u64 request_id, u8 kind, FfiServiceWorkerRegistrationSnapshot const*, bool boolean);
void web_rust_service_worker_registration_lookup_finished(JSVM*, u64 request_id, ReadonlySpan<FfiServiceWorkerRegistrationSnapshot>);
void web_rust_service_worker_registration_state_changed(JSVM*, u64 registration_id, u8 target, FfiServiceWorkerDescriptor const*);
void web_rust_service_worker_state_changed(JSVM*, FfiServiceWorkerDescriptor const*);
void web_rust_service_worker_update_found(JSVM*, u64 registration_id);
void web_rust_service_worker_ready(JSVM*, u64 request_id, FfiServiceWorkerRegistrationSnapshot const*);
void web_rust_service_worker_message_to_client(JSVM*, void* realm, FfiServiceWorkerDescriptor const* source, ReadonlyBytes origin, HTML::SerializedTransferRecord* record);
void web_rust_service_worker_enable_client_message_queue(JSVM*, void* realm);
void web_rust_service_worker_notify_controller_change(JSVM*, void* realm);
u64 web_rust_service_worker_dispatch_message_event(JSVM*, void* realm, DOM::EventTarget* global, FfiServiceWorkerClientSnapshot const*, FfiServiceWorkerDescriptor const*, ReadonlyBytes origin, HTML::SerializedTransferRecord* record, void* ctx, void (*on_pending_events_changed)(void*, bool));
void web_rust_service_worker_time_out_extended_event(JSVM*, u64 event_id);
u64 web_rust_service_worker_id(ServiceWorker const*);
void web_rust_service_worker_navigation_preload_settled(JSVM*, u64 request_id, bool succeeded, bool enabled, ReadonlyBytes header_value);
void web_rust_service_worker_clients_matched(JSVM*, u64 request_id, ReadonlySpan<FfiServiceWorkerClientSnapshot>);
void web_rust_service_worker_claim_finished(JSVM*, u64 request_id, bool succeeded);
void* web_rust_service_worker_create_clients(JSVM*, void* realm);
void* web_rust_service_worker_registration_object(JSVM*, void* realm, FfiServiceWorkerRegistrationSnapshot const*);
void* web_rust_service_worker_service_worker_object(JSVM*, void* realm, FfiServiceWorkerDescriptor const*);
}

static JS::Realm& realm_from_abi(void* realm)
{
    return *static_cast<JS::Realm*>(realm);
}

static HTML::EnvironmentSettingsObject& relevant_settings_object_of(void* realm)
{
    return HTML::relevant_settings_object(realm_from_abi(realm).global_object());
}

GC::Ref<ServiceWorkerContainer> ServiceWorkerContainer::create(JS::Realm& realm)
{
    return *static_cast<ServiceWorkerContainer*>(web_rust_service_worker_create_container(JS::HostABI::vm_to_abi(realm.vm()), &realm));
}

JSCompletion web_service_worker_get_trusted_type_compliant_script_url(void* realm, void* trusted_script_url, FlatPtr string)
{
    auto& js_realm = realm_from_abi(realm);
    Variant<GC::Ref<TrustedTypes::TrustedHTML>, GC::Ref<TrustedTypes::TrustedScript>, GC::Ref<TrustedTypes::TrustedScriptURL>, Utf16String> input = Utf16String::adopt_raw(string);
    if (trusted_script_url) {
        auto* object = Bindings::impl_from<TrustedTypes::TrustedScriptURL>(static_cast<JS::Object*>(trusted_script_url));
        VERIFY(object);
        input = GC::Ref { *object };
    }

    auto compliant_string = TrustedTypes::get_trusted_type_compliant_string(
        TrustedTypes::TrustedTypeName::TrustedScriptURL,
        js_realm.global_object(),
        move(input),
        TrustedTypes::InjectionSink::ServiceWorkerContainer_register,
        TrustedTypes::Script.view());
    if (compliant_string.is_error())
        return Bindings::completion_to_abi(js_realm, compliant_string.release_error());
    return Bindings::completion_to_abi(js_realm, JS::Value { JS::PrimitiveString::create(js_realm.vm(), compliant_string.release_value()) });
}

void web_service_worker_service_worker_client(void* realm, void* ctx, void (*on_client)(void*, ReadonlyBytes, URL::FFI::FfiOrigin const*))
{
    auto& settings = relevant_settings_object_of(realm);
    auto api_base_url = settings.api_base_url().serialize();
    URL::RustIntegration::lend_origin_to_ffi(settings.origin(), [&](URL::FFI::FfiOrigin const& origin) {
        on_client(ctx, api_base_url.bytes(), &origin);
    });
}

void web_service_worker_active_service_worker(void* realm, void* ctx, void (*on_service_worker)(void*, u64, ReadonlyBytes, u8))
{
    auto const& active_service_worker = relevant_settings_object_of(realm).active_service_worker;
    if (!active_service_worker.has_value())
        return;
    auto script_url = active_service_worker->script_url.serialize();
    on_service_worker(ctx, active_service_worker->id, script_url.bytes(), to_underlying(active_service_worker->state));
}

static bool has_dispatched_dom_content_loaded_for_service_worker_jobs(HTML::EnvironmentSettingsObject& settings)
{
    auto* window = HTML::window_from_global_object(settings.global_object());
    if (!window)
        return true;

    auto& document = window->associated_document();
    if (document.load_timing_info().dom_content_loaded_event_end_time != 0 || document.readiness() == HTML::DocumentReadyState::Complete)
        return true;

    document.set_has_service_worker_jobs_waiting_for_dom_content_loaded();
    return false;
}

void web_service_worker_schedule_job(void* realm, u64 request_id, u8 job_type, ReadonlyBytes scope_url, ReadonlyBytes script_url, u8 worker_type, u8 update_via_cache)
{
    auto& settings = relevant_settings_object_of(realm);
    ServiceWorkerJobRequest request {
        .request_id = request_id,
        .job_type = static_cast<ServiceWorkerJobType>(job_type),
        .client = settings.serialize(),
        .scope_url = URL::Parser::basic_parse(StringView { scope_url }).release_value(),
        .script_url = script_url.is_empty() ? Optional<URL::URL> {} : URL::Parser::basic_parse(StringView { script_url }),
        .worker_type = static_cast<HTML::WorkerType>(worker_type),
        .update_via_cache = static_cast<ServiceWorkerUpdateViaCache>(update_via_cache),
        .client_has_dispatched_dom_content_loaded = has_dispatched_dom_content_loaded_for_service_worker_jobs(settings),
    };
    Bindings::principal_host_defined_page(realm_from_abi(realm)).client().page_did_schedule_service_worker_job(request);
}

void web_service_worker_request_ready(void* realm, u64 request_id)
{
    auto& settings = relevant_settings_object_of(realm);
    Bindings::principal_host_defined_page(realm_from_abi(realm)).client().page_did_request_service_worker_ready(settings.id, request_id, settings.creation_url);
}

void web_service_worker_request_registrations(void* realm, u64 request_id, ReadonlyBytes client_url)
{
    auto& settings = relevant_settings_object_of(realm);
    auto& client = Bindings::principal_host_defined_page(realm_from_abi(realm)).client();
    if (client_url.is_empty()) {
        client.page_did_request_service_worker_registrations(settings.id, request_id);
        return;
    }
    client.page_did_request_service_worker_registration(settings.id, request_id, URL::Parser::basic_parse(StringView { client_url }).release_value());
}

// https://w3c.github.io/ServiceWorker/#get-frametype-algorithm
static Bindings::FrameType get_frame_type(HTML::Navigable const& navigable)
{
    // 1. If navigable's parent is not null, then return "nested".
    // NB: A navigable of a frame that another process hosts has no parent in this process, but is not a top-level
    //     traversable.
    if (!navigable.is_top_level_traversable())
        return Bindings::FrameType::Nested;

    // 2. If navigable's active browsing context is an auxiliary browsing context, then return "auxiliary".
    if (navigable.active_browsing_context_is_auxiliary())
        return Bindings::FrameType::Auxiliary;

    // 3. Return "top-level".
    return Bindings::FrameType::TopLevel;
}

// What a Client or WindowClient object that represents the settings object says of it.
static ServiceWorkerClientSnapshot client_snapshot_of(HTML::EnvironmentSettingsObject& settings)
{
    ServiceWorkerClientSnapshot snapshot {
        .id = settings.id,
        .url = settings.creation_url,
        .type = Bindings::ClientType::Worker,
        .frame_type = Bindings::FrameType::None,
        .visibility_state = Bindings::DocumentVisibilityState::Hidden,
        .focused = false,
        .ancestor_origins = {},
    };
    auto& global = settings.global_object();
    if (auto* window = HTML::window_from_global_object(global)) {
        auto& document = window->associated_document();
        snapshot.type = Bindings::ClientType::Window;
        if (auto navigable = document.navigable())
            snapshot.frame_type = get_frame_type(*navigable);
        snapshot.visibility_state = document.visibility_state();
        snapshot.focused = document.has_focus();
        if (auto ancestor_origins = document.ancestor_origins_list()) {
            for (u32 i = 0; i < ancestor_origins->length(); ++i)
                snapshot.ancestor_origins.append(ancestor_origins->item(i)->to_utf8());
        }
    } else if (Bindings::impl_from<HTML::SharedWorkerGlobalScope>(&global)) {
        snapshot.type = Bindings::ClientType::Sharedworker;
    }
    return snapshot;
}

void web_service_worker_post_message_to_service_worker(void*, u64 service_worker_id, HTML::SerializedTransferRecord* record)
{
    auto message = adopt_own(*record);

    // https://w3c.github.io/ServiceWorker/#service-worker-postmessage-options
    // 2. Let incumbentSettings be the incumbent settings object.
    // NB: The UI process substitutes the service worker for the client of a ServiceWorkerGlobalScope.
    auto& incumbent_settings = HTML::incumbent_settings_object();
    Bindings::principal_host_defined_page(incumbent_settings.realm()).client().page_did_post_message_to_service_worker(client_snapshot_of(incumbent_settings), service_worker_id, move(*message));
}

void web_service_worker_post_message_to_client(void* realm, ReadonlyBytes client_id, HTML::SerializedTransferRecord* record)
{
    auto message = adopt_own(*record);
    Bindings::principal_host_defined_page(realm_from_abi(realm)).client().page_did_post_message_to_service_worker_client(String::from_utf8_without_validation(client_id), move(*message));
}

void web_service_worker_request_navigation_preload(void* realm, u64 request_id, u64 registration_id, u8 operation, ReadonlyBytes header_value)
{
    auto& settings = relevant_settings_object_of(realm);
    Bindings::principal_host_defined_page(realm_from_abi(realm)).client().page_did_request_navigation_preload(settings.id, request_id, registration_id, static_cast<ServiceWorkerNavigationPreloadOperation>(operation), ByteString { header_value });
}

void web_service_worker_request_clients(void* realm, u64 request_id, ReadonlyBytes client_id, bool is_get, bool include_uncontrolled, u8 type)
{
    Bindings::principal_host_defined_page(realm_from_abi(realm)).client().page_did_request_service_worker_clients(request_id, String::from_utf8_without_validation(client_id), is_get, include_uncontrolled, static_cast<Bindings::ClientType>(type));
}

bool web_service_worker_claim(void* realm, u64 request_id)
{
    auto* global_scope = Bindings::impl_from<ServiceWorkerGlobalScope>(&realm_from_abi(realm).global_object());
    VERIFY(global_scope);

    // https://w3c.github.io/ServiceWorker/#clients-claim
    // 1. If the service worker is not an active worker, return a promise rejected with an "InvalidStateError"
    //    DOMException.
    if (!global_scope->is_active_worker())
        return false;
    Bindings::principal_host_defined_page(realm_from_abi(realm)).client().page_did_request_service_worker_claim(request_id);
    return true;
}

void service_worker_claim_finished(ServiceWorkerRequestId request_id, bool succeeded)
{
    web_rust_service_worker_claim_finished(JS::HostABI::vm_to_abi(JS::VM::the()), request_id, succeeded);
}

void* web_service_worker_container_of_realm(void* realm)
{
    auto& global = realm_from_abi(realm).global_object();
    if (auto* window = HTML::window_from_global_object(global))
        return window->navigator()->service_worker().ptr();
    auto* worker_global_scope = Bindings::worker_global_scope_from_global_object(global);
    VERIFY(worker_global_scope);
    return worker_global_scope->navigator()->service_worker().ptr();
}

struct SerializedRegistrationSnapshotURLs {
    String scope_url;
    String installing_worker_script_url;
    String waiting_worker_script_url;
    String active_worker_script_url;
};

static FfiServiceWorkerDescriptor descriptor_to_ffi(Optional<ServiceWorkerDescriptor> const& descriptor, String& script_url)
{
    if (!descriptor.has_value())
        return {};
    script_url = descriptor->script_url.serialize();
    return FfiServiceWorkerDescriptor {
        .exists = true,
        .id = descriptor->id,
        .script_url = script_url.bytes(),
        .state = to_underlying(descriptor->state),
    };
}

static FfiServiceWorkerRegistrationSnapshot snapshot_to_ffi(ServiceWorkerRegistrationSnapshot const& snapshot, SerializedRegistrationSnapshotURLs& urls)
{
    urls.scope_url = snapshot.scope_url.serialize();
    return FfiServiceWorkerRegistrationSnapshot {
        .id = snapshot.id,
        .scope_url = urls.scope_url.bytes(),
        .update_via_cache = to_underlying(snapshot.update_via_cache),
        .installing_worker = descriptor_to_ffi(snapshot.installing_worker, urls.installing_worker_script_url),
        .waiting_worker = descriptor_to_ffi(snapshot.waiting_worker, urls.waiting_worker_script_url),
        .active_worker = descriptor_to_ffi(snapshot.active_worker, urls.active_worker_script_url),
    };
}

void service_worker_job_settled(ServiceWorkerRequestId request_id, ServiceWorkerJobResult result)
{
    auto* vm = JS::HostABI::vm_to_abi(JS::VM::the());
    result.visit(
        [&](ServiceWorkerRegistrationSnapshot const& registration) {
            SerializedRegistrationSnapshotURLs urls;
            auto snapshot = snapshot_to_ffi(registration, urls);
            web_rust_service_worker_job_settled(vm, request_id, 0, &snapshot, false);
        },
        [&](bool value) {
            web_rust_service_worker_job_settled(vm, request_id, 1, nullptr, value);
        },
        [&](ServiceWorkerJobError error) {
            web_rust_service_worker_job_settled(vm, request_id, error == ServiceWorkerJobError::TypeError ? 2 : 3, nullptr, false);
        });
}

void service_worker_registration_lookup_finished(ServiceWorkerRequestId request_id, Vector<ServiceWorkerRegistrationSnapshot> registrations)
{
    Vector<SerializedRegistrationSnapshotURLs> urls;
    urls.resize(registrations.size());
    Vector<FfiServiceWorkerRegistrationSnapshot> snapshots;
    snapshots.ensure_capacity(registrations.size());
    for (size_t i = 0; i < registrations.size(); ++i)
        snapshots.unchecked_append(snapshot_to_ffi(registrations[i], urls[i]));
    web_rust_service_worker_registration_lookup_finished(JS::HostABI::vm_to_abi(JS::VM::the()), request_id, snapshots.span());
}

// NB: The steps that the UI process asks a process to queue a task for run in the order the process is told of them, so
//     that a change reaches the objects that the tasks of earlier messages create.
static void queue_a_task_for_state_change(GC::Ref<GC::Function<void()>> steps)
{
    HTML::queue_a_task(HTML::Task::Source::DOMManipulation, HTML::current_event_loop(), nullptr, steps);
}

void service_worker_registration_state_changed(ServiceWorkerRegistrationId registration_id, ServiceWorkerRegistrationSlot target, Optional<ServiceWorkerDescriptor> source)
{
    queue_a_task_for_state_change(GC::create_function(GC::Heap::the(), GC::captures(registration_id, target, move(source)), [](auto registration_id, auto target, auto const& source) {
        String script_url;
        auto worker = descriptor_to_ffi(source, script_url);
        web_rust_service_worker_registration_state_changed(JS::HostABI::vm_to_abi(JS::VM::the()), registration_id, to_underlying(target), &worker);
    }));
}

void service_worker_state_changed(ServiceWorkerDescriptor worker)
{
    queue_a_task_for_state_change(GC::create_function(GC::Heap::the(), GC::captures(move(worker)), [](auto const& worker) {
        String script_url;
        auto ffi_worker = descriptor_to_ffi(worker, script_url);
        web_rust_service_worker_state_changed(JS::HostABI::vm_to_abi(JS::VM::the()), &ffi_worker);
    }));
}

void service_worker_update_found(ServiceWorkerRegistrationId registration_id)
{
    queue_a_task_for_state_change(GC::create_function(GC::Heap::the(), GC::captures(registration_id), [](auto registration_id) {
        web_rust_service_worker_update_found(JS::HostABI::vm_to_abi(JS::VM::the()), registration_id);
    }));
}

void service_worker_ready(ServiceWorkerRequestId request_id, ServiceWorkerRegistrationSnapshot registration)
{
    SerializedRegistrationSnapshotURLs urls;
    auto snapshot = snapshot_to_ffi(registration, urls);
    web_rust_service_worker_ready(JS::HostABI::vm_to_abi(JS::VM::the()), request_id, &snapshot);
}

// https://w3c.github.io/ServiceWorker/#client-postmessage-options
void service_worker_message_to_client(HTML::EnvironmentId const& client_id, ServiceWorkerDescriptor source, URL::Origin const& origin, HTML::SerializedTransferRecord message)
{
    // 4.4. Let destination be the ServiceWorkerContainer object whose associated service worker client is targetClient.
    // 4.5. Add a task that runs the following steps to destination's client message queue:
    auto settings = HTML::environment_settings_object_with_id(client_id);
    if (!settings)
        return;
    String script_url;
    auto ffi_source = descriptor_to_ffi(source, script_url);
    auto serialized_origin = origin.serialize();
    web_rust_service_worker_message_to_client(JS::HostABI::vm_to_abi(JS::VM::the()), &settings->realm(), &ffi_source, serialized_origin.bytes(), new HTML::SerializedTransferRecord(move(message)));
}

void service_worker_controller_changed(HTML::EnvironmentId const& client_id, ServiceWorkerDescriptor controller)
{
    auto settings = HTML::environment_settings_object_with_id(client_id);
    if (!settings)
        return;

    // https://w3c.github.io/ServiceWorker/#activation-algorithm
    // 8.1. Set client's active worker to registration's active worker.
    // https://w3c.github.io/ServiceWorker/#clients-claim
    // 3.1.6.2. Set client's active service worker to service worker.
    settings->active_service_worker = move(controller);

    // 8.2. Invoke Notify Controller Change algorithm with client as the argument.
    web_rust_service_worker_notify_controller_change(JS::HostABI::vm_to_abi(settings->realm().vm()), &settings->realm());
}

void enable_client_message_queue(HTML::EnvironmentSettingsObject& settings)
{
    web_rust_service_worker_enable_client_message_queue(JS::HostABI::vm_to_abi(settings.realm().vm()), &settings.realm());
}

GC::Ref<ServiceWorker> service_worker_object(JS::Realm& realm, ServiceWorkerDescriptor const& worker)
{
    String script_url;
    auto ffi_worker = descriptor_to_ffi(worker, script_url);
    return *static_cast<ServiceWorker*>(web_rust_service_worker_service_worker_object(JS::HostABI::vm_to_abi(realm.vm()), &realm, &ffi_worker));
}

GC::Ref<ServiceWorkerRegistration> service_worker_registration_object(JS::Realm& realm, ServiceWorkerRegistrationSnapshot const& registration)
{
    SerializedRegistrationSnapshotURLs urls;
    auto ffi_registration = snapshot_to_ffi(registration, urls);
    return *static_cast<ServiceWorkerRegistration*>(web_rust_service_worker_registration_object(JS::HostABI::vm_to_abi(realm.vm()), &realm, &ffi_registration));
}

// https://w3c.github.io/ServiceWorker/#service-worker-postmessage-options
u64 dispatch_message_event(ServiceWorkerGlobalScope& global, ServiceWorkerMessageSource const& source, URL::Origin const& origin, HTML::SerializedTransferRecord message, void* ctx, void (*on_pending_events_changed)(void*, bool))
{
    auto& realm = HTML::relevant_realm(global);
    auto* vm = JS::HostABI::vm_to_abi(realm.vm());
    auto serialized_origin = origin.serialize();
    auto* record = new HTML::SerializedTransferRecord(move(message));
    return source.visit(
        [&](ServiceWorkerClientSnapshot const& client) {
            auto url = client.url.serialize();
            Vector<ReadonlyBytes> ancestor_origins;
            for (auto const& ancestor_origin : client.ancestor_origins)
                ancestor_origins.append(ancestor_origin.bytes());
            FfiServiceWorkerClientSnapshot ffi_client {
                .id = client.id.to_string().bytes(),
                .url = url.bytes(),
                .type = to_underlying(client.type),
                .frame_type = to_underlying(client.frame_type),
                .visibility_state = to_underlying(client.visibility_state),
                .focused = client.focused,
                .ancestor_origins = ancestor_origins.span(),
            };
            return web_rust_service_worker_dispatch_message_event(vm, &realm, &global, &ffi_client, nullptr, serialized_origin.bytes(), record, ctx, on_pending_events_changed);
        },
        [&](ServiceWorkerDescriptor const& worker) {
            String script_url;
            auto ffi_worker = descriptor_to_ffi(worker, script_url);
            return web_rust_service_worker_dispatch_message_event(vm, &realm, &global, nullptr, &ffi_worker, serialized_origin.bytes(), record, ctx, on_pending_events_changed);
        });
}

void time_out_extended_event(u64 event_id)
{
    web_rust_service_worker_time_out_extended_event(JS::HostABI::vm_to_abi(JS::VM::the()), event_id);
}

ServiceWorkerId service_worker_id(ServiceWorker const& worker)
{
    return web_rust_service_worker_id(&worker);
}

GC::Ref<Clients> create_clients(JS::Realm& realm)
{
    return *static_cast<Clients*>(web_rust_service_worker_create_clients(JS::HostABI::vm_to_abi(realm.vm()), &realm));
}

// The strings that the FFI form of client snapshots refers to.
struct SerializedClientSnapshotStrings {
    String url;
    Vector<ReadonlyBytes> ancestor_origins;
};

static FfiServiceWorkerClientSnapshot client_snapshot_to_ffi(ServiceWorkerClientSnapshot const& client, SerializedClientSnapshotStrings& strings)
{
    strings.url = client.url.serialize();
    for (auto const& ancestor_origin : client.ancestor_origins)
        strings.ancestor_origins.append(ancestor_origin.bytes());
    return FfiServiceWorkerClientSnapshot {
        .id = client.id.to_string().bytes(),
        .url = strings.url.bytes(),
        .type = to_underlying(client.type),
        .frame_type = to_underlying(client.frame_type),
        .visibility_state = to_underlying(client.visibility_state),
        .focused = client.focused,
        .ancestor_origins = strings.ancestor_origins.span(),
    };
}

void service_worker_clients_matched(ServiceWorkerRequestId request_id, Vector<ServiceWorkerClientSnapshot> const& clients)
{
    Vector<SerializedClientSnapshotStrings> strings;
    strings.resize(clients.size());
    Vector<FfiServiceWorkerClientSnapshot> ffi_clients;
    for (size_t i = 0; i < clients.size(); ++i)
        ffi_clients.append(client_snapshot_to_ffi(clients[i], strings[i]));
    web_rust_service_worker_clients_matched(JS::HostABI::vm_to_abi(JS::VM::the()), request_id, ffi_clients.span());
}

// https://w3c.github.io/ServiceWorker/#clients-matchall
Vector<ServiceWorkerClientSnapshot> service_worker_client_snapshots(Vector<HTML::EnvironmentId> const& client_ids)
{
    Vector<ServiceWorkerClientSnapshot> snapshots;
    for (auto const& client_id : client_ids) {
        auto settings = HTML::environment_settings_object_with_id(client_id);

        // 2.2.1. If client's execution ready flag is unset or client's discarded flag is set, continue.
        if (!settings || !settings->execution_ready)
            continue;

        // 2.2.2. If client is not a secure context, continue.
        if (HTML::is_non_secure_context(*settings))
            continue;

        // 2.5.1.6.2. If client is a window client and client's associated document is not browsingContext's active
        //            document, then set isClientEnumerable to false and abort these steps.
        if (auto* window = HTML::window_from_global_object(settings->global_object()); window && !window->associated_document().is_active())
            continue;

        // 2.5.1.6.3. Set windowData["frameType"] to the result of running Get Frame Type with browsingContext.
        // 2.5.1.6.4. Set windowData["visibilityState"] to browsingContext's active document's visibilityState
        //            attribute value.
        // 2.5.1.6.5. Set windowData["focusState"] to the result of running the has focus steps with browsingContext's
        //            active document as the argument.
        // 2.5.1.6.6. If client is a window client, then set windowData["ancestorOriginsList"] to browsingContext's
        //            active document's ancestor origins list's associated list.
        snapshots.append(client_snapshot_of(*settings));
    }
    return snapshots;
}

void navigation_preload_settled(ServiceWorkerRequestId request_id, Optional<ServiceWorkerNavigationPreloadState> const& state)
{
    auto* vm = JS::HostABI::vm_to_abi(JS::VM::the());
    if (!state.has_value()) {
        web_rust_service_worker_navigation_preload_settled(vm, request_id, false, false, {});
        return;
    }
    web_rust_service_worker_navigation_preload_settled(vm, request_id, true, state->enabled, state->header_value.bytes());
}

}
