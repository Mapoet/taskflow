#include "agent/tool_runtime/orphan_recovery.hpp"

namespace agent_framework::tool_runtime {
namespace {
bool lease_state(InvocationState state) {
    return state == InvocationState::Leased || state == InvocationState::Running ||
           state == InvocationState::Progressing ||
           state == InvocationState::Checkpointed ||
           state == InvocationState::Cancelling;
}
}

OrphanRecoveryReport InvocationOrphanSweeper::sweep(std::int64_t now_ms,
                                                     std::size_t limit) {
    OrphanRecoveryReport report;
    for(auto invocation : store_.recoverable(limit)) {
        ++report.inspected;
        if(!lease_state(invocation.state) || invocation.lease.fencing_token == 0 ||
           invocation.lease.expires_at_ms > now_ms) continue;
        OrphanRecoveryRecord record;
        record.invocation_id = invocation.invocation_id;
        record.prior_state = invocation.state;
        const auto old_revision = invocation.revision;
        const auto new_fence = invocation.lease.fencing_token + 1;
        invocation.revision++;
        invocation.state = InvocationState::Orphaned;
        invocation.lease.owner.clear();
        invocation.lease.instance_id.clear();
        invocation.lease.worker_generation = 0;
        invocation.lease.fencing_token = new_fence;
        invocation.lease.expires_at_ms = 0;
        InvocationEvent orphaned;
        orphaned.event_type = "invocation_orphaned_by_sweeper";
        orphaned.fencing_token = new_fence;
        orphaned.payload = {{"prior_state", name(record.prior_state)},
                            {"expired_at_ms", now_ms},
                            {"new_fencing_token", new_fence}};
        auto committed = store_.commit({invocation, old_revision, orphaned, {}, {}, {}});
        if(!committed) {
            record.error = committed.error;
            record.recovery_state = record.prior_state;
            ++report.failures;
            report.records.push_back(std::move(record));
            continue;
        }
        ++report.orphaned;
        record.fencing_token = new_fence;
        const bool must_reconcile = !invocation.idempotent ||
            !invocation.external_operation_id.empty() ||
            !invocation.provider_session_id.empty() ||
            !invocation.remote_task_id.empty();
        if(must_reconcile) {
            const auto orphan_revision = invocation.revision;
            invocation.revision++;
            invocation.state = InvocationState::Reconciling;
            InvocationEvent reconcile;
            reconcile.event_type = "invocation_reconcile_required";
            reconcile.fencing_token = new_fence;
            reconcile.payload = {{"reason", "expired_lease_with_possible_external_effect"},
                                 {"automatic_replay_allowed", false}};
            committed = store_.commit({invocation, orphan_revision, reconcile, {}, {}, {}});
            if(!committed) {
                record.error = committed.error;
                record.recovery_state = InvocationState::Orphaned;
                ++report.failures;
            } else {
                record.action = "reconcile";
                record.recovery_state = InvocationState::Reconciling;
                ++report.queued_for_reconcile;
            }
        } else {
            record.action = "takeover_ready";
            record.recovery_state = InvocationState::Orphaned;
        }
        report.records.push_back(std::move(record));
    }
    return report;
}

}  // namespace agent_framework::tool_runtime
