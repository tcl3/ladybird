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
#include <LibWebCommon/HTML/SerializationRecords.h>
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

    // The connection's process, which tells connections to one process apart.
    void const* identity() const;

    template<typename Callback>
    void visit_open_connection(Callback callback) const
    {
        m_connection.visit([&](auto const& connection) {
            if (connection && connection->is_open())
                callback(*connection);
        });
    }

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

// https://w3c.github.io/ServiceWorker/#dfn-scope-to-registration-map
class WEBVIEW_API ServiceWorkerRegistry : public Weakable<ServiceWorkerRegistry> {
    AK_MAKE_NONCOPYABLE(ServiceWorkerRegistry);
    AK_MAKE_NONMOVABLE(ServiceWorkerRegistry);

public:
    AK_ALLOC_WITH_KMALLOC;

    explicit ServiceWorkerRegistry(IsPrivate);
    ~ServiceWorkerRegistry();

    static void environment_was_discarded(Web::HTML::EnvironmentId const&);

    // https://w3c.github.io/ServiceWorker/#on-fetch-request-algorithm
    // Handle Fetch's steps 9.5 to 9.8, for the reserved client of a navigation or of a worker's script fetch.
    void match_reserved_client_of_navigation(ServiceWorkerClientConnection const&, CanonicalNavigable const&, NavigationLoader const*, Web::ServiceWorker::ServiceWorkerRequestId, Web::ServiceWorker::ServiceWorkerReservedClient const&);
    void match_reserved_client_of_worker(ServiceWorkerClientEnvironment const&, Web::ServiceWorker::ServiceWorkerRequestId, Web::ServiceWorker::ServiceWorkerReservedClient const&);

    // Whether a registry recorded the reserved client as the one the navigation's fetch matched a registration for,
    // so that the navigation's window may take the reserved client's id.
    static bool has_reserved_client_of_navigation(NavigationLoader const&, Web::HTML::EnvironmentId const&);

    // The window that the navigation created took over its reserved client. Returns the window's active service worker.
    static Optional<Web::ServiceWorker::ServiceWorkerDescriptor> reserved_client_became_window(NavigationLoader const&, Web::HTML::EnvironmentId const& reserved_client_id, Web::HTML::EnvironmentId const& window_id);

    static void navigation_loader_will_be_destroyed(NavigationLoader const&);

    void schedule_job(ServiceWorkerClientEnvironment const&, Web::ServiceWorker::ServiceWorkerJobRequest);
    void get_registration(ServiceWorkerClientEnvironment const&, Web::ServiceWorker::ServiceWorkerRequestId, URL::URL client_url);
    void get_registrations(ServiceWorkerClientEnvironment const&, Web::ServiceWorker::ServiceWorkerRequestId);
    void get_ready_registration(ServiceWorkerClientEnvironment const&, Web::ServiceWorker::ServiceWorkerRequestId, URL::URL creation_url);
    void client_did_dispatch_dom_content_loaded(Web::HTML::EnvironmentId const&);

    bool is_service_worker_agent(Web::HTML::WorkerAgentId) const;
    void service_worker_did_fetch_script(Web::HTML::WorkerAgentId, Web::ServiceWorker::ServiceWorkerScriptFetchResult);
    void service_worker_did_run(Web::HTML::WorkerAgentId, bool succeeded, Vector<String> event_types_to_handle);
    void service_worker_did_finish_lifecycle_event(Web::HTML::WorkerAgentId, Web::ServiceWorker::ServiceWorkerLifecycleEvent, bool succeeded);
    void service_worker_did_call_skip_waiting(Web::HTML::WorkerAgentId, Web::ServiceWorker::ServiceWorkerRequestId);
    void service_worker_did_change_pending_events(Web::HTML::WorkerAgentId, bool has_pending_events);
    void service_worker_did_record_scripts(Web::HTML::WorkerAgentId, Vector<Web::ServiceWorker::ServiceWorkerScriptResource>);
    void service_worker_did_respond_to_heartbeat(Web::HTML::WorkerAgentId, u64 heartbeat_id);
    void terminate_service_worker_for_testing(Web::ServiceWorker::ServiceWorkerId, bool kill_process);

    void navigation_preload(ServiceWorkerClientEnvironment const&, Web::ServiceWorker::ServiceWorkerRequestId, Web::ServiceWorker::ServiceWorkerRegistrationId, Web::ServiceWorker::ServiceWorkerNavigationPreloadOperation, ByteString header_value);
    void service_worker_did_request_claim(Web::HTML::WorkerAgentId, Web::ServiceWorker::ServiceWorkerRequestId);
    void connect_fetch_channel(ServiceWorkerClientConnection const& requester, Web::ServiceWorker::ServiceWorkerRequestId, Web::HTML::EnvironmentId const& client_id, Web::ServiceWorker::ServiceWorkerId);
    void service_worker_did_request_clients(Web::HTML::WorkerAgentId, Web::ServiceWorker::ServiceWorkerRequestId, String const& client_id, bool is_get, bool include_uncontrolled, Web::Bindings::ClientType);
    void client_did_provide_service_worker_client_snapshots(u64 gather_id, ServiceWorkerClientConnection const& provider, Vector<Web::ServiceWorker::ServiceWorkerClientSnapshot>);

    void post_message_to_service_worker(ServiceWorkerClientEnvironment const&, Web::ServiceWorker::ServiceWorkerClientSnapshot, Web::ServiceWorker::ServiceWorkerId, Web::HTML::SerializedTransferRecord);
    void service_worker_did_post_message_to_service_worker(Web::HTML::WorkerAgentId, Web::ServiceWorker::ServiceWorkerId, Web::HTML::SerializedTransferRecord);
    void service_worker_did_post_message_to_client(Web::HTML::WorkerAgentId, String const& client_id, Web::HTML::SerializedTransferRecord);
    void service_worker_process_did_exit(Web::HTML::WorkerAgentId);

    void clear_registrations_for_testing(URL::Origin const& top_level_origin);

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

    // The job that a worker's process continues, with what the job's algorithm knew when it waited for the process.
    struct JobInProgress {
        u64 job_id { 0 };
        RegistrationKey job_queue_key;
        Optional<Web::ServiceWorker::ServiceWorkerId> newest_worker;
    };

    enum class AwaitedStep : u8 {
        None,
        Start,
        Restart,
        InstallEvent,
        ActivateEvent,
    };

    // A message for a worker that is not running, which the worker gets once it runs.
    struct MessageWaitingForRun {
        Web::ServiceWorker::ServiceWorkerMessageSource source;
        URL::Origin origin;
        Web::HTML::SerializedTransferRecord message;
    };

    // https://w3c.github.io/ServiceWorker/#dfn-service-worker
    struct ServiceWorkerRecord {
        AK_ALLOC_WITH_KMALLOC;

        Web::ServiceWorker::ServiceWorkerId id { 0 };
        Web::ServiceWorker::ServiceWorkerRegistrationId registration_id { 0 };
        Partition partition;
        URL::URL script_url;
        Web::HTML::WorkerType type { Web::HTML::WorkerType::Classic };
        Web::ServiceWorker::ServiceWorkerState state { Web::ServiceWorker::ServiceWorkerState::Parsed };
        Vector<String> set_of_event_types_to_handle;
        bool skip_waiting_flag { false };

        // NB: Whether the worker's set of extended events holds an event that is active, as its process reports.
        bool has_pending_events { false };

        Optional<Web::HTML::WorkerAgentId> agent_id;
        RefPtr<WebWorkerClient> process;
        AwaitedStep awaited_step { AwaitedStep::None };
        Optional<JobInProgress> job_in_progress;
        RefPtr<Core::Timer> deadline_timer;

        // NB: What the worker runs again with, once it has installed: the settings of the client whose job created it,
        //     and the scripts its process recorded when it installed.
        Web::HTML::SerializedEnvironmentSettingsObject registering_client;
        Vector<Web::ServiceWorker::ServiceWorkerScriptResource> script_resources;
        Vector<MessageWaitingForRun> messages_waiting_for_run;

        RefPtr<Core::Timer> heartbeat_timer;
        Optional<u64> unanswered_heartbeat;
    };

    // https://w3c.github.io/ServiceWorker/#dfn-service-worker-client
    // A service worker client whose active service worker is not null, which is said to be using the active service
    // worker's containing service worker registration.
    struct ControlledClient {
        Web::HTML::EnvironmentId id;
        Web::ServiceWorker::ServiceWorkerId active_service_worker { 0 };

        // NB: The navigable whose navigation's reserved client this is, until the window the navigation creates takes
        //     the reserved client's id. The navigation's loader, which a history traversal has only once its fetch is
        //     done, and the process that runs the fetch are only compared, never dereferenced.
        Optional<Web::HTML::CrossProcessId> reserving_navigable;
        NavigationLoader const* reserving_navigation { nullptr };
        void const* reserving_process { nullptr };

        bool is_reserved() const { return reserving_navigable.has_value(); }
    };

    // The timer that stops the running workers of a partition whose last client went away.
    struct PartitionTermination {
        Partition partition;
        NonnullRefPtr<Core::Timer> timer;
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

        // https://w3c.github.io/ServiceWorker/#service-worker-registration-navigation-preload-enabled-flag
        bool navigation_preload_enabled { false };

        // https://w3c.github.io/ServiceWorker/#service-worker-registration-navigation-preload-header-value
        ByteString navigation_preload_header_value { "true"sv };
    };

    // The clients that a Clients object of a worker asked for, which the processes that host them describe.
    struct ClientsRequest {
        u64 gather_id { 0 };
        Web::ServiceWorker::ServiceWorkerId worker_id { 0 };
        Web::ServiceWorker::ServiceWorkerRequestId request_id { 0 };
        bool is_get { false };
        Vector<Web::HTML::EnvironmentId> window_clients;
        Vector<Web::HTML::EnvironmentId> worker_clients;
        Vector<void const*> processes_waited_for;
        Vector<Web::ServiceWorker::ServiceWorkerClientSnapshot> snapshots;
        RefPtr<Core::Timer> timeout;
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
        Optional<URL::URL> script_url;
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

    // A ready promise of a client that waits for a registration with an active worker.
    struct ReadyRequest {
        ServiceWorkerClientConnection connection;
        Web::HTML::EnvironmentId environment_id;
        Web::ServiceWorker::ServiceWorkerRequestId request_id { 0 };
        Partition partition;
        URL::URL creation_url;
    };

    void match_reserved_client(ServiceWorkerClientConnection const&, Optional<Partition> const&, Web::ServiceWorker::ServiceWorkerRequestId, Web::ServiceWorker::ServiceWorkerReservedClient const&, Optional<Web::HTML::CrossProcessId> reserving_navigable, NavigationLoader const* reserving_navigation);
    void forget_reserved_clients_of_navigable(Web::HTML::CrossProcessId, Optional<Web::HTML::EnvironmentId const&> except);
    bool is_using(ControlledClient const&, Registration const&) const;
    bool any_service_worker_client_is_using(Registration const&) const;
    void handle_service_worker_client_unload(Web::HTML::EnvironmentId const&);
    void forget_reserved_clients_of(NavigationLoader const&);
    void notify_controller_change(Web::HTML::EnvironmentId const&, Web::ServiceWorker::ServiceWorkerDescriptor const&);

    struct HostedClient {
        ServiceWorkerClientConnection connection;
        CanonicalEnvironmentSettingsObject const& settings;
        bool has_cross_site_ancestor { false };
        bool is_window { false };
        Optional<Web::HTML::AgentType> agent_type;
    };
    // Every window and every dedicated or shared worker environment of this registry's session.
    void for_each_hosted_client(Function<IterationDecision(HostedClient const&)> const&) const;
    Optional<ServiceWorkerClientConnection> connection_of_client(Web::HTML::EnvironmentId const&) const;

    void run_job(RegistrationKey const& job_queue_key);
    void finish_job(RegistrationKey const& job_queue_key, Job&);
    Job* job_in_progress_of(ServiceWorkerRecord const&);
    void resolve_job_promise(Job&, Web::ServiceWorker::ServiceWorkerJobResult const&);
    void reject_job_promise(Job&, Web::ServiceWorker::ServiceWorkerJobError);
    void settle_job_promise(Job&, Web::ServiceWorker::ServiceWorkerJobResult const&);

    void register_(RegistrationKey const& job_queue_key, Job&);
    void update(RegistrationKey const& job_queue_key, Job&);
    void fail_update(ServiceWorkerRecord&, Web::ServiceWorker::ServiceWorkerJobError);
    void install(ServiceWorkerRecord&);
    void finish_install(ServiceWorkerRecord&, bool install_failed);
    void activate(Registration&);
    void finish_activate(ServiceWorkerRecord&);
    void try_activate(Registration&);
    void unregister(RegistrationKey const& job_queue_key, Job&);
    void try_clear_registration(Registration&);
    void clear_registration(Registration&);
    void update_registration_state(Registration&, Web::ServiceWorker::ServiceWorkerRegistrationSlot target, Optional<Web::ServiceWorker::ServiceWorkerId> source);
    void update_worker_state(ServiceWorkerRecord&, Web::ServiceWorker::ServiceWorkerState);

    void dispatch_lifecycle_event(ServiceWorkerRecord&, Web::ServiceWorker::ServiceWorkerLifecycleEvent);
    void dispatch_message_event(Partition const& sender_partition, Web::ServiceWorker::ServiceWorkerId, Web::ServiceWorker::ServiceWorkerMessageSource, URL::Origin const&, Web::HTML::SerializedTransferRecord);
    bool run_service_worker(ServiceWorkerRecord&);
    void start_heartbeat(ServiceWorkerRecord&);
    void send_heartbeat(ServiceWorkerRecord&);
    bool has_client_in_partition(Partition const&) const;
    void stop_workers_of_partitions_without_clients();
    void stop_idle_workers_of_partition(Partition const&);
    bool service_worker_has_no_pending_events(Optional<Web::ServiceWorker::ServiceWorkerId>) const;
    bool is_unregistered(Registration const&) const;
    void terminate_service_worker(ServiceWorkerRecord&);
    void service_worker_stopped(ServiceWorkerRecord&);
    void forget_service_worker_if_unused(Web::ServiceWorker::ServiceWorkerId);
    ServiceWorkerRecord* service_worker_of_agent(Web::HTML::WorkerAgentId);
    void for_each_process_in_partition(Partition const&, Function<void(ServiceWorkerClientConnection const&)> const&) const;

    Registration& set_registration(Job const&);
    Registration* get_registration(Partition const&, URL::URL const& scope_url);
    Registration* registration_by_id(Web::ServiceWorker::ServiceWorkerRegistrationId);
    Registration* match_service_worker_registration(Partition const&, URL::URL const& client_url);
    void remove_registration(Registration&);
    Optional<Web::ServiceWorker::ServiceWorkerId> get_newest_worker(Registration const&) const;
    Web::ServiceWorker::ServiceWorkerRegistrationSnapshot snapshot_of(Registration const&) const;
    Optional<Web::ServiceWorker::ServiceWorkerDescriptor> descriptor_of(Optional<Web::ServiceWorker::ServiceWorkerId>) const;

    static Optional<Partition> partition_of(CanonicalEnvironmentSettingsObject const&, bool has_cross_site_ancestor);
    static RegistrationKey registration_key(Partition const&, URL::URL const& scope_url);

    IsPrivate m_is_private { IsPrivate::No };

    HashMap<RegistrationKey, NonnullOwnPtr<Registration>, RegistrationKeyTraits> m_registration_map;

    // NB: The registrations that Unregister removed from the registration map, until Clear Registration clears them.
    Vector<NonnullOwnPtr<Registration>> m_unregistered_registrations;

    Vector<PartitionTermination> m_partition_terminations;
    u64 m_next_heartbeat_id { 0 };

    void finish_clients_request(u64 gather_id);
    Vector<ClientsRequest> m_clients_requests;
    u64 m_next_gather_id { 0 };

    HashMap<Web::ServiceWorker::ServiceWorkerId, NonnullOwnPtr<ServiceWorkerRecord>> m_service_workers;

    // https://w3c.github.io/ServiceWorker/#dfn-scope-to-job-queue-map
    // AD-HOC: The job queues are keyed as the registrations are, so that clients in one partition never wait for the
    //         jobs of another.
    HashMap<RegistrationKey, JobQueue, RegistrationKeyTraits> m_scope_to_job_queue_map;

    Vector<ReadyRequest> m_ready_requests;

    Vector<ControlledClient> m_controlled_clients;

    Web::ServiceWorker::ServiceWorkerRegistrationId m_next_registration_id { 0 };
    Web::ServiceWorker::ServiceWorkerId m_next_service_worker_id { 0 };
    u64 m_next_job_id { 0 };
};

}
