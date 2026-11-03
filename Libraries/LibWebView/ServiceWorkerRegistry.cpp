/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibWebCommon/SecureContexts/AbstractOperations.h>
#include <LibWebView/CanonicalEnvironmentSettingsObject.h>
#include <LibWebView/ServiceWorkerRegistry.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebWorkerClient.h>
#include <LibWebView/WorkerAgentManager.h>

namespace WebView {

using namespace Web::ServiceWorker;

static constexpr auto new_service_worker_start_deadline = AK::Duration::from_seconds(5 * 60);

static Vector<ServiceWorkerRegistry*>& live_registries()
{
    static auto& registries = *new Vector<ServiceWorkerRegistry*>;
    return registries;
}

ServiceWorkerClientConnection::ServiceWorkerClientConnection(WebContentClient& client)
    : m_connection(WeakPtr<WebContentClient> { client })
{
}

ServiceWorkerClientConnection::ServiceWorkerClientConnection(WebWorkerClient& client)
    : m_connection(WeakPtr<WebWorkerClient> { client })
{
}

void ServiceWorkerClientConnection::service_worker_job_settled(ServiceWorkerRequestId request_id, ServiceWorkerJobResult const& result) const
{
    m_connection.visit([&](auto const& connection) {
        if (connection && connection->is_open())
            connection->async_service_worker_job_settled(request_id, result);
    });
}

void ServiceWorkerClientConnection::service_worker_registration_lookup_finished(ServiceWorkerRequestId request_id, Vector<ServiceWorkerRegistrationSnapshot> const& registrations) const
{
    m_connection.visit([&](auto const& connection) {
        if (connection && connection->is_open())
            connection->async_service_worker_registration_lookup_finished(request_id, registrations);
    });
}

unsigned ServiceWorkerRegistry::RegistrationKeyTraits::hash(RegistrationKey const& key)
{
    auto hash = pair_int_hash(Traits<Web::StorageAPI::StorageKey>::hash(key.partition.storage_key), key.serialized_scope_url.hash());
    if (key.partition.top_level_site.has_value())
        hash = pair_int_hash(hash, key.partition.top_level_site->hash());
    return pair_int_hash(hash, static_cast<unsigned>(key.partition.has_cross_site_ancestor));
}

ServiceWorkerRegistry::ServiceWorkerRegistry(IsPrivate is_private)
    : m_is_private(is_private)
{
    live_registries().append(this);
}

ServiceWorkerRegistry::~ServiceWorkerRegistry()
{
    live_registries().remove_first_matching([this](auto* registry) { return registry == this; });

    for (auto& updating_worker : m_updating_workers)
        WorkerAgentManager::the().close_service_worker_agent(updating_worker.agent_id);
}

void ServiceWorkerRegistry::environment_was_discarded(Web::HTML::EnvironmentId const& environment_id)
{
    for (auto* registry : live_registries())
        registry->client_did_dispatch_dom_content_loaded(environment_id);
}

Optional<ServiceWorkerRegistry::Partition> ServiceWorkerRegistry::partition_of(ServiceWorkerClientEnvironment const& client)
{
    auto storage_key = obtain_a_storage_key(client.settings);
    if (!storage_key.has_value())
        return {};
    return Partition {
        .storage_key = storage_key.release_value(),
        .top_level_site = network_isolation_top_level_site(client.settings),
        .has_cross_site_ancestor = client.has_cross_site_ancestor,
    };
}

ServiceWorkerRegistry::RegistrationKey ServiceWorkerRegistry::registration_key(Partition const& partition, URL::URL const& scope_url)
{
    return RegistrationKey {
        .partition = partition,
        .serialized_scope_url = scope_url.serialize(URL::ExcludeFragment::Yes),
    };
}

// https://w3c.github.io/ServiceWorker/#dfn-job-equivalent
bool ServiceWorkerRegistry::Job::is_equivalent_to(Job const& other) const
{
    // Two jobs are equivalent when their job type is the same and:
    if (job_type != other.job_type)
        return false;

    switch (job_type) {
    // For register and update jobs, their scope url, script url, worker type, and update via cache mode are the same.
    case ServiceWorkerJobType::Register:
    case ServiceWorkerJobType::Update:
        return scope_url == other.scope_url
            && script_url == other.script_url
            && worker_type == other.worker_type
            && update_via_cache == other.update_via_cache;
    // For unregister jobs, their scope url is the same.
    case ServiceWorkerJobType::Unregister:
        return scope_url == other.scope_url;
    }
    VERIFY_NOT_REACHED();
}

// https://w3c.github.io/ServiceWorker/#schedule-job-algorithm
void ServiceWorkerRegistry::schedule_job(ServiceWorkerClientEnvironment const& client, ServiceWorkerJobRequest request)
{
    // NB: The client's process runs the steps of register() up to the creation of its job, which is created here,
    //     from the client's environment as this process knows it.
    auto partition = partition_of(client);
    if (!partition.has_value()) {
        // AD-HOC: Obtaining a storage key can fail, which the job algorithms do not expect.
        client.connection.service_worker_job_settled(request.request_id, ServiceWorkerJobError::TypeError);
        return;
    }

    request.client.origin = client.settings.origin();
    request.client.top_level_origin = client.settings.top_level_origin();
    request.client.has_cross_site_ancestor = client.has_cross_site_ancestor;

    // https://w3c.github.io/ServiceWorker/#create-job-algorithm
    // 1. Let job be a new job.
    // 2. Set job's job type to jobType.
    // 3. Set job's storage key to storage key.
    // 4. Set job's scope url to scopeURL.
    // 5. Set job's script url to scriptURL.
    // 6. Set job's job promise to promise.
    // 7. Set job's client to client.
    // NB: The client's process holds the promise, which settles once the client learns how the job settled.
    // 8. If client is not null, set job's referrer to client's creation URL.
    // AD-HOC: Register compares only the referrer's origin, so the job keeps the origin of its client as this process
    //         knows it, rather than a creation URL that the client's process names.
    // 9. Return job.
    auto job = make<Job>(Job {
        .id = ++m_next_job_id,
        .job_type = request.job_type,
        .partition = partition.release_value(),
        .scope_url = move(request.scope_url),
        .script_url = move(request.script_url),
        .worker_type = request.worker_type,
        .update_via_cache = request.update_via_cache,
        .client = JobClient {
            .connection = client.connection,
            .environment_id = client.settings.id(),
            .request_id = request.request_id,
            .settings = move(request.client),
            .has_dispatched_dom_content_loaded = request.client_has_dispatched_dom_content_loaded,
        },
        .referrer_origin = client.settings.origin(),
        .force_bypass_cache = false,
        .list_of_equivalent_jobs = {},
        .job_promise_has_settled = false,
        .has_started = false,
    });

    // 1. Let jobQueue be null.
    // 2. Let jobScope be job's scope url, serialized.
    auto job_scope = registration_key(job->partition, job->scope_url);

    // 3. If scope to job queue map[jobScope] does not exist, set scope to job queue map[jobScope] to a new job queue.
    // 4. Set jobQueue to scope to job queue map[jobScope].
    auto& job_queue = m_scope_to_job_queue_map.ensure(job_scope);

    // 5. If jobQueue is empty, then:
    if (job_queue.is_empty()) {
        // 1. Set job's containing job queue to jobQueue, and enqueue job to jobQueue.
        job_queue.append(move(job));

        // 2. Invoke Run Job with jobQueue.
        run_job(job_scope);
        return;
    }

    // 6. Else:
    // 1. Let lastJob be the element at the back of jobQueue.
    auto& last_job = *job_queue.last();

    // 2. If job is equivalent to lastJob and lastJob's job promise has not settled, append job to lastJob's list of
    //    equivalent jobs.
    if (job->is_equivalent_to(last_job) && !last_job.job_promise_has_settled) {
        last_job.list_of_equivalent_jobs.append(move(job));
        return;
    }

    // 3. Else, set job's containing job queue to jobQueue, and enqueue job to jobQueue.
    job_queue.append(move(job));
}

// https://w3c.github.io/ServiceWorker/#run-job-algorithm
void ServiceWorkerRegistry::run_job(RegistrationKey const& job_queue_key)
{
    // 1. Assert: jobQueue is not empty.
    VERIFY(!m_scope_to_job_queue_map.get(job_queue_key)->is_empty());

    // 2. Queue a task to run these steps:
    Core::deferred_invoke([weak_this = make_weak_ptr(), job_queue_key] {
        auto* registry = weak_this.ptr();
        if (!registry)
            return;
        auto job_queue = registry->m_scope_to_job_queue_map.get(job_queue_key);
        if (!job_queue.has_value() || job_queue->is_empty())
            return;

        // 1. Let job be the first item in jobQueue.
        auto& job = *job_queue->first();
        if (job.has_started)
            return;

        switch (job.job_type) {
        // 2. If job's job type is register, run Register with job in parallel.
        case ServiceWorkerJobType::Register:
        // 3. Else if job's job type is update, run Update with job in parallel.
        case ServiceWorkerJobType::Update:
            // NOTE: For a register job and an update job, the user agent delays queuing a task for running the job until
            //       after a DOMContentLoaded event has been dispatched to the document that initiated the job.
            // NB: Jobs behind one that waits wait with it. Once its client reports DOMContentLoaded or is discarded,
            //     client_did_dispatch_dom_content_loaded() runs the job.
            if (job.client.has_value() && !job.client->has_dispatched_dom_content_loaded)
                return;
            job.has_started = true;
            if (job.job_type == ServiceWorkerJobType::Register)
                registry->register_(job_queue_key, job);
            else
                registry->update(job_queue_key, job);
            break;

        // 4. Else if job's job type is unregister, run Unregister with job in parallel.
        case ServiceWorkerJobType::Unregister:
            // FIXME: Run Unregister. Until it runs, the job fails.
            job.has_started = true;
            registry->reject_job_promise(job, ServiceWorkerJobError::TypeError);
            registry->finish_job(job_queue_key, job);
            break;
        }
    });
}

void ServiceWorkerRegistry::client_did_dispatch_dom_content_loaded(Web::HTML::EnvironmentId const& environment_id)
{
    Vector<RegistrationKey> job_queues_to_run;
    for (auto& [job_queue_key, job_queue] : m_scope_to_job_queue_map) {
        for (auto& job : job_queue) {
            if (job->client.has_value() && job->client->environment_id == environment_id)
                job->client->has_dispatched_dom_content_loaded = true;
        }
        if (!job_queue.is_empty() && !job_queue.first()->has_started)
            job_queues_to_run.append(job_queue_key);
    }

    for (auto const& job_queue_key : job_queues_to_run)
        run_job(job_queue_key);
}

// https://w3c.github.io/ServiceWorker/#finish-job-algorithm
void ServiceWorkerRegistry::finish_job(RegistrationKey const& job_queue_key, Job& job)
{
    // 1. Let jobQueue be job's containing job queue.
    auto& job_queue = *m_scope_to_job_queue_map.get(job_queue_key);

    // 2. Assert: the first item in jobQueue is job.
    VERIFY(job_queue.first().ptr() == &job);

    // 3. Dequeue from jobQueue.
    (void)job_queue.take_first();

    // 4. If jobQueue is not empty, invoke Run Job with jobQueue.
    if (!job_queue.is_empty())
        run_job(job_queue_key);
    else
        m_scope_to_job_queue_map.remove(job_queue_key);
}

void ServiceWorkerRegistry::settle_job_promise(Job& job, ServiceWorkerJobResult const& result)
{
    if (job.job_promise_has_settled)
        return;
    job.job_promise_has_settled = true;
    if (job.client.has_value())
        job.client->connection.service_worker_job_settled(job.client->request_id, result);
}

// https://w3c.github.io/ServiceWorker/#resolve-job-promise-algorithm
void ServiceWorkerRegistry::resolve_job_promise(Job& job, ServiceWorkerJobResult const& value)
{
    // 1. If job's client is not null, queue a task, on job's client's responsible event loop using the DOM manipulation
    //    task source, to run the following substeps:
    // NB: The client's process queues the task, and runs its substeps, when it learns that the job promise settled.
    settle_job_promise(job, value);

    // 2. For each equivalentJob in job's list of equivalent jobs:
    for (auto& equivalent_job : job.list_of_equivalent_jobs) {
        // 1. If equivalentJob's client is null, continue to the next iteration of the loop.
        // 2. Queue a task, on equivalentJob's client's responsible event loop using the DOM manipulation task source, to
        //    run the following substeps:
        settle_job_promise(*equivalent_job, value);
    }
}

// https://w3c.github.io/ServiceWorker/#reject-job-promise-algorithm
void ServiceWorkerRegistry::reject_job_promise(Job& job, ServiceWorkerJobError error_data)
{
    // 1. If job's client is not null, queue a task, on job's client's responsible event loop using the DOM manipulation
    //    task source, to reject job's job promise with a new exception with errorData, in job's client's Realm.
    settle_job_promise(job, error_data);

    // 2. For each equivalentJob in job's list of equivalent jobs:
    for (auto& equivalent_job : job.list_of_equivalent_jobs) {
        // 1. If equivalentJob's client is null, continue.
        // 2. Queue a task, on equivalentJob's client's responsible event loop using the DOM manipulation task source, to
        //    reject equivalentJob's job promise with a new exception with errorData, in equivalentJob's client's Realm.
        settle_job_promise(*equivalent_job, error_data);
    }
}

// https://w3c.github.io/ServiceWorker/#register-algorithm
void ServiceWorkerRegistry::register_(RegistrationKey const& job_queue_key, Job& job)
{
    auto script_origin = job.script_url.origin();
    auto scope_origin = job.scope_url.origin();

    // 1. If the result of running potentially trustworthy origin with the origin of job's script url as the argument is
    //    Not Trusted, then:
    if (Web::SecureContexts::is_origin_potentially_trustworthy(script_origin) == Web::SecureContexts::Trustworthiness::NotTrustworthy) {
        // 1. Invoke Reject Job Promise with job and "SecurityError" DOMException.
        reject_job_promise(job, ServiceWorkerJobError::SecurityError);

        // 2. Invoke Finish Job with job and abort these steps.
        finish_job(job_queue_key, job);
        return;
    }

    // 2. If job's script url's origin and job's referrer's origin are not same origin, then:
    if (!script_origin.is_same_origin(job.referrer_origin)) {
        // 1. Invoke Reject Job Promise with job and "SecurityError" DOMException.
        reject_job_promise(job, ServiceWorkerJobError::SecurityError);

        // 2. Invoke Finish Job with job and abort these steps.
        finish_job(job_queue_key, job);
        return;
    }

    // 3. If job's scope url's origin and job's referrer's origin are not same origin, then:
    if (!scope_origin.is_same_origin(job.referrer_origin)) {
        // 1. Invoke Reject Job Promise with job and "SecurityError" DOMException.
        reject_job_promise(job, ServiceWorkerJobError::SecurityError);

        // 2. Invoke Finish Job with job and abort these steps.
        finish_job(job_queue_key, job);
        return;
    }

    // 4. Let registration be the result of running Get Registration given job's storage key and job's scope url.
    auto* registration = get_registration(job.partition, job.scope_url);

    // 5. If registration is not null, then:
    if (registration) {
        // 1. Let newestWorker be the result of running the Get Newest Worker algorithm passing registration as the
        //    argument.
        auto newest_worker = get_newest_worker(*registration);

        // 2. If newestWorker is not null, job's script url equals newestWorker's script url, job's worker type equals
        //    newestWorker's type, and job's update via cache mode's value equals registration's update via cache mode,
        //    then:
        if (newest_worker.has_value()) {
            auto const& worker = m_service_workers.get(*newest_worker).value();
            if (job.script_url == worker.script_url && job.worker_type == worker.type && job.update_via_cache == registration->update_via_cache) {
                // 1. Invoke Resolve Job Promise with job and registration.
                resolve_job_promise(job, snapshot_of(*registration));

                // 2. Invoke Finish Job with job and abort these steps.
                finish_job(job_queue_key, job);
                return;
            }
        }
    }
    // 6. Else:
    else {
        // 1. Invoke Set Registration algorithm with job's storage key, job's scope url, and job's update via cache mode.
        set_registration(job);
    }

    // 7. Invoke Update algorithm passing job as the argument.
    update(job_queue_key, job);
}

// https://w3c.github.io/ServiceWorker/#update-algorithm
void ServiceWorkerRegistry::update(RegistrationKey const& job_queue_key, Job& job)
{
    // 1. Let registration be the result of running Get Registration given job's storage key and job's scope url.
    auto* registration = get_registration(job.partition, job.scope_url);

    // 2. If registration is null, then:
    if (!registration) {
        // 1. Invoke Reject Job Promise with job and TypeError.
        reject_job_promise(job, ServiceWorkerJobError::TypeError);

        // 2. Invoke Finish Job with job and abort these steps.
        finish_job(job_queue_key, job);
        return;
    }

    // 3. Let newestWorker be the result of running Get Newest Worker algorithm passing registration as the argument.
    auto newest_worker = get_newest_worker(*registration);

    // 4. If job's job type is update, and newestWorker is not null and its script url does not equal job's script url,
    //    then:
    if (job.job_type == ServiceWorkerJobType::Update && newest_worker.has_value() && m_service_workers.get(*newest_worker)->script_url != job.script_url) {
        // 1. Invoke Reject Job Promise with job and TypeError.
        reject_job_promise(job, ServiceWorkerJobError::TypeError);

        // 2. Invoke Finish Job with job and abort these steps.
        finish_job(job_queue_key, job);
        return;
    }

    // 5. Let hasUpdatedResources be false.
    // 6. Let updatedResourceMap be an ordered map where the keys are URLs and the values are responses.
    // 7. Switching on job's worker type, run these substeps with the following options:
    // NB: The service worker this creates runs in a process of its own, which runs these steps and the perform the
    //     fetch hook with the environment settings object it creates for the worker. It reports how they end to
    //     continue_update().
    ServiceWorkerRecord worker {
        .id = ++m_next_service_worker_id,
        .registration_id = registration->id,
        .script_url = job.script_url,
        .type = job.worker_type,
        .state = ServiceWorkerState::Parsed,
    };

    auto fail_to_start = [&] {
        reject_job_promise(job, ServiceWorkerJobError::TypeError);
        if (!newest_worker.has_value())
            remove_registration(*registration);
        finish_job(job_queue_key, job);
    };

    if (!job.client.has_value()) {
        fail_to_start();
        return;
    }

    auto agent = WorkerAgentManager::the().start_service_worker_agent(m_is_private, job.client->settings.origin, job.client->settings.top_level_origin, job.client->settings.has_cross_site_ancestor);
    if (agent.is_error()) {
        // NB: A service worker the system cannot give a process fails, as one whose script cannot be fetched does.
        dbgln("Unable to start a service worker process: {}", agent.error());
        fail_to_start();
        return;
    }

    ServiceWorkerStartRequest start_request {
        .worker = ServiceWorkerDescriptor {
            .id = worker.id,
            .script_url = worker.script_url,
            .state = worker.state,
        },
        .worker_type = worker.type,
        .registration = snapshot_of(*registration),
        .environment_id = agent.value().environment_id,
        .origin = job.client->settings.origin,
        .has_cross_site_ancestor = job.client->settings.has_cross_site_ancestor,
        .top_level_origin = job.client->settings.top_level_origin,
        .script_fetch = ServiceWorkerScriptFetchParameters {
            .job_client = job.client->settings,
            .force_bypass_cache = job.force_bypass_cache,
        },
    };
    agent.value().client->async_start_service_worker(start_request);

    auto agent_id = agent.value().id;
    auto start_deadline_timer = Core::Timer::create_single_shot(new_service_worker_start_deadline, [weak_this = make_weak_ptr(), agent_id] {
        if (auto* registry = weak_this.ptr())
            registry->continue_update(agent_id, ServiceWorkerJobError::TypeError);
    });
    start_deadline_timer->start();

    m_updating_workers.append(UpdatingWorker {
        .worker = move(worker),
        .job_id = job.id,
        .job_queue_key = job_queue_key,
        .newest_worker = newest_worker,
        .agent_id = agent_id,
        .process = agent.value().client,
        .start_deadline_timer = move(start_deadline_timer),
    });
}

bool ServiceWorkerRegistry::is_service_worker_agent(Web::HTML::WorkerAgentId agent_id) const
{
    return any_of(m_updating_workers, [&](auto const& updating_worker) { return updating_worker.agent_id == agent_id; });
}

void ServiceWorkerRegistry::service_worker_did_fetch_script(Web::HTML::WorkerAgentId agent_id, ServiceWorkerScriptFetchResult result)
{
    continue_update(agent_id, result);
}

void ServiceWorkerRegistry::service_worker_process_did_exit(Web::HTML::WorkerAgentId agent_id)
{
    continue_update(agent_id, ServiceWorkerJobError::TypeError);
}

void ServiceWorkerRegistry::stop_updating_worker(UpdatingWorker& updating_worker)
{
    if (updating_worker.start_deadline_timer)
        updating_worker.start_deadline_timer->stop();
    WorkerAgentManager::the().close_service_worker_agent(updating_worker.agent_id);
}

// https://w3c.github.io/ServiceWorker/#update-algorithm
void ServiceWorkerRegistry::continue_update(Web::HTML::WorkerAgentId agent_id, ServiceWorkerScriptFetchResult result)
{
    auto index = m_updating_workers.find_first_index_if([&](auto const& updating_worker) { return updating_worker.agent_id == agent_id; });
    if (!index.has_value())
        return;

    auto updating_worker = m_updating_workers.take(*index);
    // NB: The deadline timer may be what continues the update, so it outlives its own handler.
    Core::deferred_invoke([timer = move(updating_worker.start_deadline_timer)] { });
    stop_updating_worker(updating_worker);

    auto job_queue = m_scope_to_job_queue_map.get(updating_worker.job_queue_key);
    VERIFY(job_queue.has_value() && !job_queue->is_empty());
    auto& job = *job_queue->first();
    VERIFY(job.id == updating_worker.job_id);

    auto* registration = get_registration(job.partition, job.scope_url);
    VERIFY(registration);

    // When the algorithm asynchronously completes, continue the rest of these steps, with script being the asynchronous
    // completion value.

    // 8. If script is null or Is Async Module with script's record, script's base URL, and « » is true, then:
    if (result.has_value()) {
        // 1. Invoke Reject Job Promise with job and TypeError.
        // NOTE: This will do nothing if Reject Job Promise was previously invoked with "SecurityError" DOMException.
        // NB: The worker's process reports the exception the job promise was first rejected with.
        reject_job_promise(job, *result);

        // 2. If newestWorker is null, then remove registration map[(registration's storage key, serialized scopeURL)].
        if (!updating_worker.newest_worker.has_value())
            remove_registration(*registration);

        // 3. Invoke Finish Job with job and abort these steps.
        finish_job(updating_worker.job_queue_key, job);
        return;
    }

    // FIXME: 9. If hasUpdatedResources is false, then:
    // FIXME:    1. Set registration's update via cache mode to job's update via cache mode.
    // FIXME:    2. Invoke Resolve Job Promise with job and registration.
    // FIXME:    3. Invoke Finish Job with job and abort these steps.

    // 10. Let worker be a new service worker.
    // 11. Set worker's script url to job's script url, worker's script resource to script, worker's type to job's worker
    //     type, and worker's script resource map to updatedResourceMap.
    // 12. Append url to worker's set of used scripts.
    // 13. Set worker's script resource's policy container to policyContainer.
    // NB: The worker's process holds its script resource and script resource map.

    // 14. Let forceBypassCache be true if job's force bypass cache flag is set, and false otherwise.
    // FIXME: 15. Let runResult be the result of running the Run Service Worker algorithm with worker and forceBypassCache.
    // NB: Until Run Service Worker runs the script, its result is failure.

    // 16. If runResult is failure or an abrupt completion, then:
    {
        // 1. Invoke Reject Job Promise with job and TypeError.
        reject_job_promise(job, ServiceWorkerJobError::TypeError);

        // 2. If newestWorker is null, then remove registration map[(registration's storage key, serialized scopeURL)].
        if (!updating_worker.newest_worker.has_value())
            remove_registration(*registration);

        // 3. Invoke Finish Job with job.
        finish_job(updating_worker.job_queue_key, job);
    }

    // FIXME: 17. Else, invoke Install algorithm with job, worker, and registration as its arguments.
}

// https://w3c.github.io/ServiceWorker/#set-registration-algorithm
ServiceWorkerRegistry::Registration& ServiceWorkerRegistry::set_registration(Job const& job)
{
    // 1. Run the following steps atomically.
    // 2. Let scopeString be serialized scope with the exclude fragment flag set.
    auto key = registration_key(job.partition, job.scope_url);

    // 3. Let registration be a new service worker registration whose storage key is set to storage key, scope url is
    //    set to scope, and update via cache mode is set to updateViaCache.
    auto registration = make<Registration>(Registration {
        .id = ++m_next_registration_id,
        .key = key,
        .scope_url = job.scope_url,
        .update_via_cache = job.update_via_cache,
        .installing_worker = {},
        .waiting_worker = {},
        .active_worker = {},
    });
    auto& result = *registration;

    // 4. Set registration map[(storage key, scopeString)] to registration.
    m_registration_map.set(move(key), move(registration));

    // 5. Return registration.
    return result;
}

// https://w3c.github.io/ServiceWorker/#get-registration-algorithm
ServiceWorkerRegistry::Registration* ServiceWorkerRegistry::get_registration(Partition const& partition, URL::URL const& scope_url)
{
    // 1. Run the following steps atomically.
    // 2. Let scopeString be the empty string.
    // 3. If scope is not null, set scopeString to serialized scope with the exclude fragment flag set.
    // 4. For each (entry storage key, entry scope) → registration of registration map:
    //    1. If storage key equals entry storage key and scopeString matches entry scope, then return registration.
    // 5. Return null.
    auto registration = m_registration_map.get(registration_key(partition, scope_url));
    return registration.value_or(nullptr);
}

// https://w3c.github.io/ServiceWorker/#scope-match-algorithm
ServiceWorkerRegistry::Registration* ServiceWorkerRegistry::match_service_worker_registration(Partition const& partition, URL::URL const& client_url)
{
    // 1. Run the following steps atomically.
    // 2. Let clientURLString be serialized clientURL.
    auto client_url_string = client_url.serialize();

    // 3. Let matchingScopeString be the empty string.
    Optional<String> matching_scope_string;

    // 4. Let scopeStringSet be an empty list.
    // 5. For each (entry storage key, entry scope) of registration map's keys:
    //    1. If storage key equals entry storage key, then append entry scope to the end of scopeStringSet.
    // 6. Set matchingScopeString to the longest value in scopeStringSet which the value of clientURLString starts with,
    //    if it exists.
    for (auto const& [key, registration] : m_registration_map) {
        if (key.partition != partition || !client_url_string.starts_with_bytes(key.serialized_scope_url))
            continue;
        if (!matching_scope_string.has_value() || key.serialized_scope_url.bytes().size() > matching_scope_string->bytes().size())
            matching_scope_string = key.serialized_scope_url;
    }

    // 7. Let matchingScope be null.
    // 8. If matchingScopeString is not the empty string, then:
    //    1. Set matchingScope to the result of parsing matchingScopeString.
    //    2. Assert: matchingScope's origin and clientURL's origin are same origin.
    // 9. Return the result of running Get Registration given storage key and matchingScope.
    if (!matching_scope_string.has_value())
        return nullptr;
    auto registration = m_registration_map.get(RegistrationKey { partition, matching_scope_string.release_value() });
    VERIFY(registration.has_value());
    VERIFY((*registration)->scope_url.origin().is_same_origin(client_url.origin()));
    return *registration;
}

void ServiceWorkerRegistry::remove_registration(Registration& registration)
{
    m_registration_map.remove(registration.key);
}

// https://w3c.github.io/ServiceWorker/#get-newest-worker-algorithm
Optional<ServiceWorkerId> ServiceWorkerRegistry::get_newest_worker(Registration const& registration) const
{
    // 1. Run the following steps atomically.
    // 2. Let newestWorker be null.
    // 3. If registration's installing worker is not null, set newestWorker to registration's installing worker.
    if (registration.installing_worker.has_value())
        return registration.installing_worker;

    // 4. Else if registration's waiting worker is not null, set newestWorker to registration's waiting worker.
    if (registration.waiting_worker.has_value())
        return registration.waiting_worker;

    // 5. Else if registration's active worker is not null, set newestWorker to registration's active worker.
    // 6. Return newestWorker.
    return registration.active_worker;
}

Optional<ServiceWorkerDescriptor> ServiceWorkerRegistry::descriptor_of(Optional<ServiceWorkerId> worker_id) const
{
    if (!worker_id.has_value())
        return {};
    auto const& worker = m_service_workers.get(*worker_id).value();
    return ServiceWorkerDescriptor {
        .id = worker.id,
        .script_url = worker.script_url,
        .state = worker.state,
    };
}

ServiceWorkerRegistrationSnapshot ServiceWorkerRegistry::snapshot_of(Registration const& registration) const
{
    return ServiceWorkerRegistrationSnapshot {
        .id = registration.id,
        .scope_url = registration.scope_url,
        .update_via_cache = registration.update_via_cache,
        .installing_worker = descriptor_of(registration.installing_worker),
        .waiting_worker = descriptor_of(registration.waiting_worker),
        .active_worker = descriptor_of(registration.active_worker),
    };
}

// https://w3c.github.io/ServiceWorker/#navigator-service-worker-getRegistration
void ServiceWorkerRegistry::get_registration(ServiceWorkerClientEnvironment const& client, ServiceWorkerRequestId request_id, URL::URL client_url)
{
    // NB: The client's process runs the other steps, and settles its promise with what step 8.1 finds here.
    // 2. Let storage key be the result of running obtain a storage key given client.
    auto partition = partition_of(client);

    // 8. Run the following substeps in parallel:
    // 1. Let registration be the result of running Match Service Worker Registration given storage key and clientURL.
    Vector<ServiceWorkerRegistrationSnapshot> registrations;
    if (partition.has_value()) {
        if (auto* registration = match_service_worker_registration(*partition, client_url))
            registrations.append(snapshot_of(*registration));
    }

    // 2. Queue a task on promise's relevant settings object's responsible event loop, using the DOM manipulation task
    //    source, to run the following steps:
    client.connection.service_worker_registration_lookup_finished(request_id, registrations);
}

// https://w3c.github.io/ServiceWorker/#navigator-service-worker-getRegistrations
void ServiceWorkerRegistry::get_registrations(ServiceWorkerClientEnvironment const& client, ServiceWorkerRequestId request_id)
{
    // NB: The client's process runs the other steps, and settles its promise with what step 4 finds here.
    // 2. Let client storage key be the result of running obtain a storage key given client.
    auto partition = partition_of(client);

    // 4. Run the following steps in parallel:
    // 1. Let registrations be a new list.
    Vector<Registration const*> registrations;

    // 2. For each (storage key, scope) → registration of registration map:
    //    1. If storage key equals client storage key, then append registration to registrations.
    if (partition.has_value()) {
        for (auto const& [key, registration] : m_registration_map) {
            if (key.partition == *partition)
                registrations.append(registration.ptr());
        }
    }

    // NB: The registration map is ordered by when each registration was set.
    quick_sort(registrations, [](auto const* a, auto const* b) { return a->id < b->id; });

    Vector<ServiceWorkerRegistrationSnapshot> snapshots;
    snapshots.ensure_capacity(registrations.size());
    for (auto const* registration : registrations)
        snapshots.unchecked_append(snapshot_of(*registration));

    // 3. Queue a task on promise's relevant settings object's responsible event loop, using the DOM manipulation task
    //    source, to run the following steps:
    client.connection.service_worker_registration_lookup_finished(request_id, snapshots);
}

}
