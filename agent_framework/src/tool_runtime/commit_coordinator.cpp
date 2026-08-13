#include "agent/tool_runtime/commit_coordinator.hpp"
#include "agent/contracts/contract.hpp"
namespace agent_framework::tool_runtime
{
    InvocationCommitCoordinator::InvocationCommitCoordinator(InvocationStore &i, distributed::ObjectStore &o, ToolEffectJournal &e, run::RunStore &r, harness::CrossStoreCoordinator &c,FaultInjector f) : invocations_(i), objects_(o), effects_(e), runs_(r), coordination_(c),fault_(std::move(f)) {}
    InvocationCommitOutcome InvocationCommitCoordinator::reserve(const InvocationCommitRequest&r){InvocationCommitOutcome out;auto invocation=invocations_.load(r.invocation_id);if(!invocation){out.error="invocation not found";return out;}if(invocation->lease.fencing_token&&invocation->lease.fencing_token!=r.fencing_token){out.error="stale fencing token";return out;}if(auto current=effects_.find_idempotency(r.idempotency_key)){if(current->request_digest!=r.request_digest){out.error="effect request digest conflict";return out;}out.committed=true;return out;}ToolEffectRecord record;record.task_id=invocation->metadata.identity.task_id;record.session_id=invocation->conversation_id;record.tool_name=invocation->tool_name;record.tool_call_id=invocation->tool_call_id;record.idempotency_key=r.idempotency_key;record.request_digest=r.request_digest;record.safe_to_replay=r.idempotent;record.reconciliation_policy=r.idempotent?ToolReconciliationPolicy::ReplayIdempotent:ToolReconciliationPolicy::ManualReview;auto begun=effects_.begin(record);out.committed=begun==ToolEffectBeginResult::Started||begun==ToolEffectBeginResult::ExistingInFlight||begun==ToolEffectBeginResult::ExistingCommitted;if(!out.committed)out.error="effect reservation rejected";if(fault_)fault_("after_input_reservation");return out;}
    InvocationCommitOutcome InvocationCommitCoordinator::commit(const InvocationCommitRequest &r) { return drive(r, false); }
    InvocationCommitOutcome InvocationCommitCoordinator::reconcile(const InvocationCommitRequest &r) { return drive(r, true); }
    InvocationCommitOutcome InvocationCommitCoordinator::drive(const InvocationCommitRequest &r, bool recovery)
    {
        InvocationCommitOutcome out;
        auto invocation = invocations_.load(r.invocation_id);
        if (!invocation)
        {
            out.error = "invocation not found";
            return out;
        }
        if (invocation->lease.fencing_token && invocation->lease.fencing_token != r.fencing_token)
        {
            out.error = "stale fencing token";
            return out;
        }
        if (!r.effect_known && !r.idempotent)
        {
            if (!effects_.find_idempotency(r.idempotency_key))
            {
                ToolEffectRecord record;
                record.task_id = invocation->metadata.identity.task_id;
                record.session_id = invocation->conversation_id;
                record.tool_name = invocation->tool_name;
                record.tool_call_id = invocation->tool_call_id;
                record.idempotency_key = r.idempotency_key;
                record.request_digest = r.request_digest;
                record.reconciliation_policy = ToolReconciliationPolicy::ManualReview;
                (void)effects_.begin(record);
            }
            effects_.mark_manual_review(r.idempotency_key, "unknown_non_idempotent_effect");
            auto prior = invocation->revision;
            invocation->revision++;
            invocation->state = InvocationState::ManualReview;
            InvocationEvent event;
            event.event_type = "invocation_manual_review";
            event.fencing_token = r.fencing_token;
            event.payload = {{"reason", "unknown_non_idempotent_effect"}};
            auto saved = invocations_.commit({*invocation, prior, event, {}, {}, {}});
            out.manual_review = bool(saved);
            out.error = "unknown non-idempotent effect";
            return out;
        }
        auto reserved=reserve(r);if(!reserved.committed){out.error=reserved.error;return out;}
        auto canonical = contracts::canonical_digest(r.result);
        if (!canonical)
        {
            out.error = "result digest unavailable";
            return out;
        }
        out.result_digest = *canonical;
        const auto bytes = r.result.dump();
        std::string error;
        auto artifact = objects_.put(r.tenant_id, bytes, r.result_media_type, {}, &error);
        if (!artifact)
        {
            out.error = "artifact write/verify failed: " + error;
            return out;
        }
        out.artifact_digest = artifact->digest;
        if(fault_)fault_("after_artifact_put");
        auto verified_bytes = objects_.get(*artifact, &error);
        if (!verified_bytes || *verified_bytes != bytes)
        {
            out.error = "artifact read-after-write verification failed: " + error;
            return out;
        }
        auto effect = effects_.find_idempotency(r.idempotency_key);
        if (effect && effect->request_digest != r.request_digest)
        {
            out.error = "effect request digest conflict";
            return out;
        }
        effect = effects_.find_idempotency(r.idempotency_key);
        if (effect && effect->status == ToolEffectStatus::Started && !effects_.complete(r.idempotency_key, out.artifact_digest))
        {
            out.error = "effect completion failed";
            return out;
        }
        if(fault_)fault_("after_effect_complete");
        if (effect && effect->status == ToolEffectStatus::Committed)
        {
        }
        else if (!effects_.commit(r.idempotency_key))
        {
            out.error = "effect commit failed";
            return out;
        }
        if(fault_)fault_("after_effect_commit");
        auto run_effect = runs_.effect(r.run_id, r.invocation_id);
        if (run_effect)
        {
            if (run_effect->request_digest != r.request_digest || run_effect->fencing_token != r.fencing_token)
            {
                out.error = "run effect pin conflict";
                return out;
            }
            if (run_effect->state == run::EffectState::Prepared)
            {
                auto advanced = runs_.advance_effect(r.run_id, r.invocation_id, run::EffectState::Prepared, run::EffectState::Committed, out.artifact_digest);
                if (!advanced)
                {
                    out.error = advanced.error;
                    return out;
                }
                if(fault_)fault_("after_run_effect_commit");
            }
        }
        else
        {
            out.error = "prepared run effect missing";
            return out;
        }
        harness::CrossStoreOperation op;
        op.operation_id = r.invocation_id + ":effect-commit";
        op.tenant_id = r.tenant_id;
        op.run_id = r.run_id;
        op.harness_id = r.harness_id;
        op.operation_kind = "invocation_effect_commit";
        op.idempotency_key = r.idempotency_key;
        op.policy_revision = "invocation-commit-v1";
        op.expected_refs = {{"artifact_digest", out.artifact_digest}, {"canonical_result_digest", out.result_digest}, {"effect_receipt_digest", out.artifact_digest}};
        if (!coordination_.execute(op, &error) && !coordination_.reconcile(op.operation_id, &error))
        {
            out.error = "cross-store coordination failed: " + error;
            return out;
        }
        if(fault_)fault_("after_cross_store_confirm");
        out.coordination_receipt = op.operation_id;
        if (invocation->state == InvocationState::CompletedCandidate || invocation->state == InvocationState::Reconciling)
        {
            auto prior = invocation->revision;
            invocation->revision++;
            invocation->state = InvocationState::EffectCommitted;
            InvocationEvent event;
            event.event_type = recovery ? "invocation_effect_reconciled" : "invocation_effect_committed";
            event.fencing_token = r.fencing_token;
            event.payload = {{"artifact_digest", out.artifact_digest}, {"canonical_result_digest", out.result_digest}, {"coordination_receipt", out.coordination_receipt}};
            InvocationReceipt receipt{out.result_digest, out.artifact_digest, "", "", true};
            auto saved = invocations_.commit({*invocation, prior, event, {}, {}, receipt});
            if (!saved)
            {
                out.error = saved.error;
                return out;
            }
        }
        out.committed = true;
        return out;
    }
    std::size_t InvocationCommitCoordinator::sweep_orphans(const std::function<std::optional<InvocationCommitRequest>(const ToolEffectRecord&)>&resolve,std::size_t limit){std::size_t handled=0;for(const auto&record:effects_.recoverable()){if(handled>=limit)break;auto request=resolve?resolve(record):std::nullopt;if(!request){effects_.mark_manual_review(record.idempotency_key,"orphan_input_unavailable");handled++;continue;}auto outcome=reconcile(*request);if(outcome.committed||outcome.manual_review)handled++;}return handled;}
}
