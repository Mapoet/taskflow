#include "agent/tool_runtime/worker_runtime.hpp"
namespace agent_framework::tool_runtime
{
    LeaseWorkerRuntime::LeaseWorkerRuntime(InvocationStore &s, distributed::SQLiteDurableQueue &q, distributed::SQLiteWorkerRegistry &w, WorkerIdentity id, std::int64_t lease) : store_(s), queue_(q), workers_(w), worker_(std::move(id)), lease_ms_(lease)
    {
        if (worker_.worker_id.empty() || worker_.instance_id.empty() || worker_.generation == 0 || lease_ms_ <= 0)
            throw std::invalid_argument("valid worker identity and lease required");
    }
    StoreResult LeaseWorkerRuntime::advance(LongRunningToolInvocation &v, InvocationState state, std::string event, std::uint64_t fence, nlohmann::json payload)
    {
        auto prior = v.revision;
        v.revision++;
        v.state = state;
        InvocationEvent e;
        e.event_type = std::move(event);
        e.fencing_token = fence;
        e.payload = std::move(payload);
        auto r = store_.commit({v, prior, std::move(e), {}, {}, {}});
        if (!r)
            v.revision = prior;
        return r;
    }
    StoreResult LeaseWorkerRuntime::enqueue(LongRunningToolInvocation v)
    {
        auto created = store_.create(v);
        if (!created && created.status != InvocationStoreStatus::AlreadyExists)
            return created;
        if (v.state == InvocationState::Created)
        {
            auto r = advance(v, InvocationState::Admitted, "invocation_admitted", 0);
            if (!r)
                return r;
        }
        if (v.state == InvocationState::Admitted)
        {
            auto r = advance(v, InvocationState::Queued, "invocation_queued", 0);
            if (!r)
                return r;
        }
        distributed::QueueTask task;
        task.task_id = v.invocation_id;
        task.tenant_id = v.metadata.identity.tenant_id;
        task.idempotency_key = v.invocation_id;
        task.payload_digest = v.input_digest;
        task.max_attempts = 3;
        std::string error;
        if (!queue_.enqueue(task, &error))
            return {InvocationStoreStatus::Error, v.revision, error};
        return {InvocationStoreStatus::Committed, v.revision, {}};
    }
    std::optional<ClaimedInvocation> LeaseWorkerRuntime::claim(std::string_view tenant, std::int64_t now, std::string *error)
    {
        auto lease = queue_.claim_with_quota(worker_.worker_id, tenant, now, lease_ms_);
        if (!lease)
            return {};
        auto v = store_.load(lease->task.task_id);
        if (!v)
        {
            queue_.nack_with_quota(lease->task.task_id, worker_.worker_id, lease->fencing_token, now);
            if (error)
                *error = "invocation missing";
            return {};
        }
        if (v->lease.fencing_token != 0 && lease->fencing_token > v->lease.fencing_token &&
            (v->state == InvocationState::Leased || v->state == InvocationState::Running ||
             v->state == InvocationState::Progressing || v->state == InvocationState::Checkpointed))
        {
            v->lease = {worker_.worker_id, worker_.instance_id, worker_.generation,
                        lease->fencing_token, now + lease_ms_};
            auto orphaned = advance(*v, InvocationState::Orphaned, "invocation_orphaned",
                                    lease->fencing_token,
                                    {{"takeover", true}, {"fencing_token", lease->fencing_token}});
            if (!orphaned || !advance(*v, InvocationState::Queued, "invocation_requeued_after_takeover",
                                      lease->fencing_token))
            {
                if (error)
                    *error = orphaned ? "takeover requeue failed" : orphaned.error;
                return {};
            }
        }
        v->attempt = lease->task.attempts;
        v->lease = {worker_.worker_id, worker_.instance_id, worker_.generation, lease->fencing_token, now + lease_ms_};
        auto r = advance(*v, InvocationState::Leased, "invocation_leased", lease->fencing_token, {{"owner", worker_.worker_id}, {"fencing_token", lease->fencing_token}});
        if (!r)
        {
            queue_.nack_with_quota(lease->task.task_id, worker_.worker_id, lease->fencing_token, now);
            if (error)
                *error = r.error;
            return {};
        }
        r = advance(*v, InvocationState::Running, "invocation_running", lease->fencing_token);
        if (!r)
        {
            if (error)
                *error = r.error;
            return {};
        }
        return ClaimedInvocation{std::move(*v), std::move(*lease)};
    }
    bool LeaseWorkerRuntime::renew(const ClaimedInvocation &c, std::int64_t now, std::string *error)
    {
        if (!workers_.heartbeat(worker_.worker_id, worker_.instance_id, worker_.generation, now))
        {
            if (error)
                *error = "worker heartbeat rejected";
            return false;
        }
        if (!queue_.renew(c.invocation.invocation_id, worker_.worker_id, c.queue_lease.fencing_token, now, lease_ms_))
        {
            if (error)
                *error = "queue lease renewal rejected";
            return false;
        }
        return true;
    }
    bool LeaseWorkerRuntime::complete(ClaimedInvocation &c, InvocationReceipt receipt, std::int64_t now, std::string *error)
    {
        (void)now;
        auto state = receipt.effect_known ? InvocationState::CompletedCandidate : InvocationState::Reconciling;
        auto prior = c.invocation.revision;
        c.invocation.revision++;
        c.invocation.state = state;
        InvocationEvent e;
        e.event_type = receipt.effect_known ? "invocation_completed_candidate" : "invocation_reconciling";
        e.fencing_token = c.queue_lease.fencing_token;
        e.payload = {{"result_digest", receipt.result_digest}, {"effect_known", receipt.effect_known}};
        auto r = store_.commit({c.invocation, prior, std::move(e), {}, {}, receipt});
        if (!r)
        {
            c.invocation.revision = prior;
            if (error)
                *error = r.error;
            return false;
        }
        if (receipt.effect_known && !queue_.ack_with_quota(c.invocation.invocation_id, worker_.worker_id, c.queue_lease.fencing_token))
        {
            if (error)
                *error = "queue ack rejected";
            return false;
        }
        return true;
    }
    bool LeaseWorkerRuntime::fail(ClaimedInvocation &c, std::string_view code, std::int64_t available, std::string *error)
    {
        auto r = advance(c.invocation, InvocationState::Retrying, "invocation_retrying", c.queue_lease.fencing_token, {{"error_code", code}});
        if (!r)
        {
            if (error)
                *error = r.error;
            return false;
        }
        if (!queue_.nack_with_quota(c.invocation.invocation_id, worker_.worker_id, c.queue_lease.fencing_token, available))
        {
            if (error)
                *error = "queue nack rejected";
            return false;
        }
        return true;
    }
    namespace
    {
        std::string restart_name(RestartPolicy p)
        {
            switch (p)
            {
            case RestartPolicy::Attach:
                return "attach";
            case RestartPolicy::RestartFromCheckpoint:
                return "restart_from_checkpoint";
            case RestartPolicy::RestartFromInput:
                return "restart_from_input";
            default:
                return "manual_review";
            }
        }
    }
    std::optional<ExecutionHandle> LeaseWorkerRuntime::start(ClaimedInvocation &c, ExecutionAdapter &a, const nlohmann::json &input, std::string *error)
    {
        ExecutionRequest request{c.invocation, input, c.invocation.invocation_id, c.invocation.checkpoint_ref, c.queue_lease.fencing_token};
        auto handle = a.start(request, error);
        if (!handle)
            return {};
        c.invocation.adapter_id = a.id();
        c.invocation.adapter_revision = a.revision();
        c.invocation.adapter_generation = a.deployment_generation();
        c.invocation.external_operation_id = handle->external_id;
        c.invocation.adapter_restart_policy = restart_name(a.restart_policy());
        auto saved = advance(c.invocation, InvocationState::Progressing, "invocation_adapter_started", c.queue_lease.fencing_token, {{"adapter_id", a.id()}, {"adapter_revision", a.revision()}, {"adapter_generation", a.deployment_generation()}, {"external_operation_id", handle->external_id}, {"restart_policy", c.invocation.adapter_restart_policy}});
        if (!saved)
        {
            if (error)
                *error = saved.error;
            return {};
        }
        return handle;
    }
    ExecutionObservation LeaseWorkerRuntime::observe(ClaimedInvocation &c, ExecutionAdapter &a, const ExecutionHandle &h, std::string *error)
    {
        if (h.fencing_token != c.queue_lease.fencing_token)
        {
            if (error)
                *error = "stale adapter handle fencing token";
            return {ObservationState::Unknown, {}, {}, {}, "stale_fencing_token", false, false};
        }
        auto observation = a.query(h);
        if (observation.state == ObservationState::Progress && c.invocation.state != InvocationState::Progressing)
            advance(c.invocation, InvocationState::Progressing, "invocation_adapter_progress", h.fencing_token, {{"checkpoint_ref", observation.checkpoint_ref}});
        return observation;
    }
    std::optional<ExecutionHandle> LeaseWorkerRuntime::recover(ClaimedInvocation &c, const ExecutionAdapterRegistry &registry, const nlohmann::json &input, std::string *error)
    {
        auto a = registry.find(c.invocation.adapter_id, c.invocation.adapter_revision, c.invocation.adapter_generation);
        if (!a)
        {
            if (error)
                *error = "pinned adapter unavailable";
            return {};
        }
        ExecutionRequest request{c.invocation, input, c.invocation.invocation_id, c.invocation.external_operation_id, c.queue_lease.fencing_token};
        if (a->restart_policy() == RestartPolicy::Attach)
            return a->attach(request, error);
        if (a->restart_policy() == RestartPolicy::RestartFromCheckpoint || a->restart_policy() == RestartPolicy::RestartFromInput)
        {
            if (!c.invocation.idempotent)
            {
                if (error)
                    *error = "non-idempotent invocation cannot restart";
                return {};
            }
            return a->start(request, error);
        }
        if (error)
            *error = "adapter recovery requires manual review";
        return {};
    }
}
