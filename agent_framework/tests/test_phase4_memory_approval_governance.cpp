#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <filesystem>
#include <memory>

#include "agent/approval/executor.hpp"
#include "agent/internal/platform_io.hpp"
#include "agent/memory_v2/governance.hpp"

namespace {
using namespace agent_framework;
memory_v2::MemoryRecord record() {
    memory_v2::MemoryRecord out;
    out.metadata.identity.tenant_id = "tenant-a";
    out.metadata.identity.task_id = "task-a";
    out.metadata.identity.memory_id = "memory-a";
    out.record_id = "memory-a";
    out.scope.tenant_id = "tenant-a";
    out.scope.project_id = "project-a";
    out.scope.level = memory_v2::MemoryLevel::Project;
    out.kind = memory_v2::MemoryKind::Semantic;
    out.authority = memory_v2::Authority::Candidate;
    out.status = memory_v2::MemoryStatus::Candidate;
    out.source_kind = "verified-evidence";
    out.source_digest = "sha256:source";
    out.content_type = "application/json";
    out.content = {{"fact", "v1"}};
    return out;
}
approval::ApprovalRequest request(const memory_v2::MemoryRecord& record, std::string id) {
    approval::ApprovalRequest out;
    out.metadata = record.metadata;
    out.approval_id = std::move(id);
    out.request_kind = "memory_governance";
    out.requester_id = "memory-agent";
    out.scope = "memory:" + record.record_id;
    out.reason = "verified memory governance change";
    out.risk_level = "medium";
    out.policy_revision = "phase4-policy-v1";
    out.plan_digest = "sha256:memory-plan";
    out.arguments_digest = "sha256:memory-action";
    out.artifact_digest = memory_v2::encode(record).at("canonical_digest");
    out.memory_view_digest = "sha256:view";
    out.created_at = "2026-08-11T00:00:00Z";
    out.expires_at = "2026-08-12T00:00:00Z";
    return out;
}
approval::ApprovalReview review(const approval::ApprovalRequest& request) {
    approval::ApprovalReview out;
    out.approval_id = request.approval_id;
    out.request_digest = approval::encode(request).at("canonical_digest");
    out.reviewer.principal_id = "memory-reviewer";
    out.reviewer.roles = {"approver"};
    out.decision = approval::Decision::Approved;
    out.reason = "evidence and propagation impact reviewed";
    out.decided_at = "2026-08-11T01:00:00Z";
    return out;
}
class Sink final : public memory_v2::ForgetSink {
public:
    std::string id() const override { return "vector-index"; }
    bool erase(std::string_view id, std::string*) override { erased = std::string(id); return true; }
    std::string erased;
};
}

int main() {
    using namespace agent_framework;
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("phase4-memory-approval-" + std::to_string(internal::current_process_id()));
    std::error_code ec; fs::remove_all(root, ec); fs::create_directories(root);
    auto memories = std::make_shared<memory_v2::SQLiteMemoryStore>((root / "memory.sqlite3").string());
    approval::SQLiteApprovalStore approvals((root / "approval.sqlite3").string());
    approval::AccountableApprovalExecutor executor((root / "votes.sqlite3").string(), approvals);
    memory_v2::ApprovalBoundMemoryGovernance governance(memories, approvals);
    auto value = record(); assert(memories->append(value));

    auto promotion = request(value, "approval-promote");
    assert(approvals.put_request(promotion));
    assert(executor.review(review(promotion), 0).outcome == approval::ReviewOutcome::Approved);
    assert(governance.promote(value.record_id, 1, memory_v2::MemoryStatus::Verified,
        memory_v2::Authority::Verified, "evidence-verification-1", promotion.approval_id,
        "2026-08-11T02:00:00Z"));
    auto promoted = memories->current(value.record_id); assert(promoted && promoted->revision == 2);

    // An approval is bound to the exact pre-change digest and cannot authorize a later revision.
    auto replay = governance.forget(value.record_id, 2, promotion.approval_id,
                                    "2026-08-11T02:00:00Z");
    assert(replay.commit.status == memory_v2::CommitStatus::Forbidden);
    assert(replay.commit.error == "memory_approval_binding_mismatch");

    auto correction_request = request(*promoted, "approval-correct");
    assert(approvals.put_request(correction_request));
    assert(executor.review(review(correction_request), 0).outcome == approval::ReviewOutcome::Approved);
    auto corrected = *promoted; corrected.content = {{"fact", "corrected-v2"}};
    assert(governance.correct(corrected, 2, {"evidence-correction-1"},
                              correction_request.approval_id, "2026-08-11T02:00:00Z"));
    auto revision3 = memories->current(value.record_id); assert(revision3 && revision3->revision == 3);
    assert(revision3->metadata.extensions.at("correction_evidence_ids").size() == 1);

    auto forget_request = request(*revision3, "approval-forget");
    assert(approvals.put_request(forget_request));
    assert(executor.review(review(forget_request), 0).outcome == approval::ReviewOutcome::Approved);
    auto sink = std::make_shared<Sink>(); assert(governance.register_forget_sink(sink));
    auto forgotten = governance.forget(value.record_id, 3, forget_request.approval_id,
                                       "2026-08-11T02:00:00Z");
    assert(forgotten.commit);
    assert(forgotten.completed_sinks == std::vector<std::string>{"vector-index"});
    assert(sink->erased == value.record_id);
    assert(memories->current(value.record_id)->status == memory_v2::MemoryStatus::Tombstoned);
    fs::remove_all(root, ec);
    return 0;
}
