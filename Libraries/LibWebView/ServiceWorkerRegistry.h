/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <LibCore/Forward.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWebCommon/HTML/Scripting/EnvironmentId.h>
#include <LibWebCommon/HTML/WorkerAgentForward.h>
#include <LibWebCommon/ServiceWorker/ServiceWorkerTypes.h>
#include <LibWebCommon/StorageAPI/StorageKey.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

class WEBVIEW_API ServiceWorkerClientConnection {
public:
    explicit ServiceWorkerClientConnection(WebContentClient&);
    explicit ServiceWorkerClientConnection(WebWorkerClient&);

    void service_worker_job_settled(Web::ServiceWorker::ServiceWorkerRequestId, Web::ServiceWorker::ServiceWorkerJobResult const&) const;
    void service_worker_registration_lookup_finished(Web::ServiceWorker::ServiceWorkerRequestId, Vector<Web::ServiceWorker::ServiceWorkerRegistrationSnapshot> const&) const;

private:
    Variant<WeakPtr<WebContentClient>, WeakPtr<WebWorkerClient>> m_connection;
};

struct ServiceWorkerClientEnvironment {
    ServiceWorkerClientConnection connection;
    CanonicalEnvironmentSettingsObject const& settings;
    bool has_cross_site_ancestor { false };
};

// https://w3c.github.io/ServiceWorker/#dfn-registration-map
class WEBVIEW_API ServiceWorkerRegistry : public Weakable<ServiceWorkerRegistry> {
    AK_MAKE_NONCOPYABLE(ServiceWorkerRegistry);
    AK_MAKE_NONMOVABLE(ServiceWorkerRegistry);

public:
    AK_ALLOC_WITH_KMALLOC;

    explicit ServiceWorkerRegistry(IsPrivate);
    ~ServiceWorkerRegistry();

    static void environment_was_discarded(Web::HTML::EnvironmentId const&);

    void schedule_job(ServiceWorkerClientEnvironment const&, Web::ServiceWorker::ServiceWorkerJobRequest);
    void get_registration(ServiceWorkerClientEnvironment const&, Web::ServiceWorker::ServiceWorkerRequestId, URL::URL client_url);
    void get_registrations(ServiceWorkerClientEnvironment const&, Web::ServiceWorker::ServiceWorkerRequestId);
    void client_did_dispatch_dom_content_loaded(Web::HTML::EnvironmentId const&);

    bool is_service_worker_agent(Web::HTML::WorkerAgentId) const;
    void service_worker_did_fetch_script(Web::HTML::WorkerAgentId, Web::ServiceWorker::ServiceWorkerScriptFetchResult);
    void service_worker_process_did_exit(Web::HTML::WorkerAgentId);

private:
    // AD-HOC: Registrations are keyed by the network state partition of the clients that use them as well as by their
    //         storage key, so that a registration made under one top-level site is invisible under another.
    struct Partition {
        Web::StorageAPI::StorageKey storage_key;
        Optional<Utf16String> top_level_site;
        bool has_cross_site_ancestor { false };

        bool operator==(Partition const&) const = default;
    };

    struct RegistrationKey {
        Partition partition;
        String serialized_scope_url;

        bool operator==(RegistrationKey const&) const = default;
    };

    struct RegistrationKeyTraits : public DefaultTraits<RegistrationKey> {
        static unsigned hash(RegistrationKey const&);
    };

    // https://w3c.github.io/ServiceWorker/#dfn-service-worker
    struct ServiceWorkerRecord {
        Web::ServiceWorker::ServiceWorkerId id { 0 };
        Web::ServiceWorker::ServiceWorkerRegistrationId registration_id { 0 };
        URL::URL script_url;
        Web::HTML::WorkerType type { Web::HTML::WorkerType::Classic };
        Web::ServiceWorker::ServiceWorkerState state { Web::ServiceWorker::ServiceWorkerState::Parsed };
    };

    // https://w3c.github.io/ServiceWorker/#dfn-service-worker-registration
    struct Registration {
        AK_ALLOC_WITH_KMALLOC;

        Web::ServiceWorker::ServiceWorkerRegistrationId id { 0 };
        RegistrationKey key;
        URL::URL scope_url;
        Web::ServiceWorker::ServiceWorkerUpdateViaCache update_via_cache { Web::ServiceWorker::ServiceWorkerUpdateViaCache::Imports };
        Optional<Web::ServiceWorker::ServiceWorkerId> installing_worker;
        Optional<Web::ServiceWorker::ServiceWorkerId> waiting_worker;
        Optional<Web::ServiceWorker::ServiceWorkerId> active_worker;
    };

    struct JobClient {
        ServiceWorkerClientConnection connection;
        Web::HTML::EnvironmentId environment_id;
        Web::ServiceWorker::ServiceWorkerRequestId request_id { 0 };
        Web::HTML::SerializedEnvironmentSettingsObject settings;
        bool has_dispatched_dom_content_loaded { false };
    };

    // https://w3c.github.io/ServiceWorker/#dfn-job
    struct Job {
        AK_ALLOC_WITH_KMALLOC;

        u64 id { 0 };
        Web::ServiceWorker::ServiceWorkerJobType job_type { Web::ServiceWorker::ServiceWorkerJobType::Register };
        Partition partition;
        URL::URL scope_url;
        URL::URL script_url;
        Web::HTML::WorkerType worker_type { Web::HTML::WorkerType::Classic };
        Web::ServiceWorker::ServiceWorkerUpdateViaCache update_via_cache { Web::ServiceWorker::ServiceWorkerUpdateViaCache::Imports };
        Optional<JobClient> client;
        URL::Origin referrer_origin;
        bool force_bypass_cache { false };
        Vector<NonnullOwnPtr<Job>> list_of_equivalent_jobs;
        bool job_promise_has_settled { false };
        bool has_started { false };

        bool is_equivalent_to(Job const&) const;
    };

    using JobQueue = Vector<NonnullOwnPtr<Job>>;

    struct UpdatingWorker {
        ServiceWorkerRecord worker;
        u64 job_id { 0 };
        RegistrationKey job_queue_key;
        Optional<Web::ServiceWorker::ServiceWorkerId> newest_worker;
        Web::HTML::WorkerAgentId agent_id { 0 };
        RefPtr<WebWorkerClient> process;
        RefPtr<Core::Timer> start_deadline_timer;
    };

    void run_job(RegistrationKey const& job_queue_key);
    void finish_job(RegistrationKey const& job_queue_key, Job&);
    void resolve_job_promise(Job&, Web::ServiceWorker::ServiceWorkerJobResult const&);
    void reject_job_promise(Job&, Web::ServiceWorker::ServiceWorkerJobError);
    void settle_job_promise(Job&, Web::ServiceWorker::ServiceWorkerJobResult const&);

    void register_(RegistrationKey const& job_queue_key, Job&);
    void update(RegistrationKey const& job_queue_key, Job&);
    void continue_update(Web::HTML::WorkerAgentId, Web::ServiceWorker::ServiceWorkerScriptFetchResult);
    void stop_updating_worker(UpdatingWorker&);

    Registration& set_registration(Job const&);
    Registration* get_registration(Partition const&, URL::URL const& scope_url);
    Registration* match_service_worker_registration(Partition const&, URL::URL const& client_url);
    void remove_registration(Registration&);
    Optional<Web::ServiceWorker::ServiceWorkerId> get_newest_worker(Registration const&) const;
    Web::ServiceWorker::ServiceWorkerRegistrationSnapshot snapshot_of(Registration const&) const;
    Optional<Web::ServiceWorker::ServiceWorkerDescriptor> descriptor_of(Optional<Web::ServiceWorker::ServiceWorkerId>) const;

    static Optional<Partition> partition_of(ServiceWorkerClientEnvironment const&);
    static RegistrationKey registration_key(Partition const&, URL::URL const& scope_url);

    IsPrivate m_is_private { IsPrivate::No };

    HashMap<RegistrationKey, NonnullOwnPtr<Registration>, RegistrationKeyTraits> m_registration_map;
    HashMap<Web::ServiceWorker::ServiceWorkerId, ServiceWorkerRecord> m_service_workers;

    // https://w3c.github.io/ServiceWorker/#dfn-scope-to-job-queue-map
    // AD-HOC: The job queues are keyed as the registrations are, so that clients in one partition never wait for the
    //         jobs of another.
    HashMap<RegistrationKey, JobQueue, RegistrationKeyTraits> m_scope_to_job_queue_map;

    Vector<UpdatingWorker> m_updating_workers;

    Web::ServiceWorker::ServiceWorkerRegistrationId m_next_registration_id { 0 };
    Web::ServiceWorker::ServiceWorkerId m_next_service_worker_id { 0 };
    u64 m_next_job_id { 0 };
};

}
