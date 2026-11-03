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
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/ServiceWorker/ServiceWorkerContainer.h>
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

// Defined on the Rust side.
void* web_rust_service_worker_create_container(JSVM*, void* realm);
void web_rust_service_worker_job_settled(JSVM*, u64 request_id, u8 kind, FfiServiceWorkerRegistrationSnapshot const*, bool boolean);
void web_rust_service_worker_registration_lookup_finished(JSVM*, u64 request_id, ReadonlySpan<FfiServiceWorkerRegistrationSnapshot>);
void web_rust_service_worker_registration_state_changed(JSVM*, u64 registration_id, u8 target, FfiServiceWorkerDescriptor const*);
void web_rust_service_worker_state_changed(JSVM*, FfiServiceWorkerDescriptor const*);
void web_rust_service_worker_update_found(JSVM*, u64 registration_id);
void web_rust_service_worker_ready(JSVM*, u64 request_id, FfiServiceWorkerRegistrationSnapshot const*);
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

}
