#include "agent/tool_runtime/worker_runtime.hpp"
#include <chrono>
namespace agent_framework::tool_runtime
{
    LeaseWorkerRuntime::LeaseWorkerRuntime(InvocationStore &s, distributed::SQLiteDurableQueue &q, distributed::SQLiteWorkerRegistry &w, WorkerIdentity id, std::int64_t lease, ExecutionControlStore* controls) : store_(s), queue_(q), workers_(w), worker_(std::move(id)), lease_ms_(lease), controls_(controls)
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
        if (controls_)
        {
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            ExecutionControlEnvelope control{v.invocation_id, v.metadata.identity.tenant_id,
                v.budget.wall_time_ms ? now + static_cast<std::int64_t>(v.budget.wall_time_ms) : 0, 0,
                {v.budget.output_bytes ? v.budget.output_bytes : 4U * 1024U * 1024U,
                 v.budget.progress_events ? v.budget.progress_events : 1024U,
                 BackpressureMode::Block}};
            auto created_control = controls_->create(control);
            if (!created_control && created_control.error != "already exists")
                return {InvocationStoreStatus::Error, v.revision, created_control.error};
        }
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
        if (controls_)
        {
            if (auto intent = controls_->load(c.invocation.invocation_id))
                request.control = intent->control;
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (request.control.deadline_at_ms && request.control.deadline_at_ms <= now)
            {
                if (error) *error = "execution deadline exceeded before adapter start";
                return {};
            }
        }
        auto handle = a.start(request, error);
        if (!handle)
            return {};
        c.invocation.adapter_id = a.id();
        c.invocation.adapter_revision = a.revision();
        c.invocation.adapter_generation = a.deployment_generation();
        c.invocation.external_operation_id = handle->external_id;
        c.invocation.provider_session_id = handle->provider_session_id;
        c.invocation.remote_task_id = handle->remote_task_id;
        c.invocation.adapter_peer_id = handle->peer_id;
        c.invocation.adapter_restart_policy = restart_name(a.restart_policy());
        auto saved = advance(c.invocation, InvocationState::Progressing, "invocation_adapter_started", c.queue_lease.fencing_token, {{"adapter_id", a.id()}, {"adapter_revision", a.revision()}, {"adapter_generation", a.deployment_generation()}, {"external_operation_id", handle->external_id},{"provider_session_id",handle->provider_session_id},{"remote_task_id",handle->remote_task_id},{"peer_id",handle->peer_id}, {"restart_policy", c.invocation.adapter_restart_policy}});
        if (!saved)
        {
            if (error)
                *error = saved.error;
            return {};
        }
        return handle;
    }
    bool LeaseWorkerRuntime::request_cancel(ClaimedInvocation &c, ExecutionControlStore &controls, std::string reason, std::int64_t now, std::string *error)
    {
        auto result = controls.request_cancel(c.invocation.invocation_id, std::move(reason), now);
        if (!result)
        {
            if (error)
                *error = result.error;
            return false;
        }
        auto saved = advance(c.invocation, InvocationState::Cancelling, "invocation_cancel_requested", c.queue_lease.fencing_token, {{"cancel_generation", result.generation}});
        if (!saved && error)
            *error = saved.error;
        return bool(saved);
    }
    bool LeaseWorkerRuntime::drive_cancel(ClaimedInvocation &c, ExecutionAdapter &adapter, const ExecutionHandle &handle, ExecutionControlStore &controls, std::int64_t now, std::int64_t escalation, std::string *error)
    {
        auto intent = controls.claim(c.invocation.invocation_id, worker_.worker_id, now, lease_ms_);
        if (!intent)
        {
            if (error)
                *error = "cancellation intent unavailable";
            return false;
        }
        CancellationResult result;
        CancellationStage attempted = CancellationStage::Cooperative;
        if (intent->stage == CancellationStage::Cooperative) attempted = CancellationStage::Terminate;
        else if (intent->stage == CancellationStage::Terminate) attempted = CancellationStage::Kill;
        else if (intent->stage == CancellationStage::Kill || intent->stage == CancellationStage::Reconciling)
            attempted = CancellationStage::Reconciling;
        if (attempted == CancellationStage::Reconciling)
        {
            ExecutionRequest request{c.invocation, {}, c.invocation.invocation_id,
                                     c.invocation.external_operation_id, c.queue_lease.fencing_token,
                                     intent->control};
            auto reconciled = adapter.reconcile(request, handle);
            const auto& observation = reconciled.observation;
            result = {observation.state == ObservationState::Cancelled,
                      observation.state == ObservationState::Cancelled,
                      observation.error_code, observation.effect_known,
                      observation.result_digest};
        }
        else result = adapter.escalate(handle, attempted);
        const auto next = result.terminal && result.effect_known ? CancellationStage::Cancelled :
                          attempted == CancellationStage::Reconciling ? CancellationStage::ManualReview :
                          result.accepted ? attempted : CancellationStage::Reconciling;
        auto advanced = controls.advance(c.invocation.invocation_id, worker_.worker_id, intent->fencing_token, intent->revision, next, now + escalation, result.effect_known, result.receipt_digest);
        if (!advanced)
        {
            if (error)
                *error = advanced.error;
            return false;
        }
        if (next == CancellationStage::Cancelled)
        {
            auto saved = advance(c.invocation, InvocationState::Cancelled, "invocation_cancelled", c.queue_lease.fencing_token, {{"receipt_digest", result.receipt_digest}});
            if (!saved)
            {
                if (error)
                    *error = saved.error;
                return false;
            }
            return queue_.ack_with_quota(c.invocation.invocation_id, worker_.worker_id, c.queue_lease.fencing_token);
        }
        if (next == CancellationStage::Cooperative || next == CancellationStage::Terminate ||
            next == CancellationStage::Kill)
            return true;
        const auto invocation_state = next == CancellationStage::ManualReview ? InvocationState::ManualReview :
                                      next == CancellationStage::Reconciling ? InvocationState::Reconciling : InvocationState::Cancelling;
        auto saved = advance(c.invocation, invocation_state,
                             next == CancellationStage::ManualReview ? "invocation_cancel_manual_review" :
                             next == CancellationStage::Reconciling ? "invocation_cancel_reconciling" : "invocation_cancel_escalated",
                             c.queue_lease.fencing_token,
                             {{"diagnostic", result.diagnostic}, {"effect_known", result.effect_known}, {"stage", std::string(name(next))}});
        if (!saved && error)
            *error = saved.error;
        return bool(saved);
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
    bool LeaseWorkerRuntime::persist_observation(ClaimedInvocation &c, ExecutionObservation &o, IncrementalResultStore &streams, IncrementalStreamKind kind, std::string_view key, std::string *error)
    {
        if (key.empty())
        {
            if (error)
                *error = "incremental observation idempotency key required";
            return false;
        }
        const auto tenant = c.invocation.metadata.identity.tenant_id;
        const auto stream = c.invocation.invocation_id + ":" + std::string(name(kind));
        IncrementalOpenRequest request{tenant, stream, c.invocation.metadata.identity.run_id, c.invocation.invocation_id, std::to_string(c.invocation.attempt), "application/json", kind};
        auto opened = streams.open(request);
        if (!opened)
        {
            if (error)
                *error = opened.error;
            return false;
        }
        const auto payload = o.result.dump();
        BackpressureReservation reservation;
        if (controls_)
        {
            reservation = controls_->reserve(c.invocation.invocation_id, payload.size(), 1,
                                               !o.information_gain);
            if (!reservation.accepted)
            {
                if (reservation.dropped) return true;
                if (error) *error = reservation.error;
                return false;
            }
        }
        auto appended = streams.append({tenant, stream, std::string(key), payload, opened.manifest.revision});
        if (!appended)
        {
            if (controls_) controls_->release(c.invocation.invocation_id, payload.size(), 1);
            if (error)
                *error = appended.error;
            return false;
        }
        auto ref = partial_result_ref(appended.manifest, o.information_gain);
        o.incremental_result = ref;
        auto prior = c.invocation.revision;
        c.invocation.revision++;
        InvocationEvent event;
        event.event_type = "invocation_incremental_result";
        event.fencing_token = c.queue_lease.fencing_token;
        event.information_gain = o.information_gain;
        event.payload = {{"uri", ref.uri}, {"manifest_digest", ref.digest}, {"size", ref.size}};
        auto committed = store_.commit({c.invocation, prior, std::move(event), {}, ref, {}});
        if (!committed)
        {
            c.invocation.revision = prior;
            if (error)
                *error = committed.error;
            return false;
        }
        if (controls_ && !controls_->release(c.invocation.invocation_id, payload.size(), 1))
        {
            if (error) *error = "backpressure reservation release failed";
            return false;
        }
        return true;
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
        if (controls_)
            if (auto intent = controls_->load(c.invocation.invocation_id)) request.control = intent->control;
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
