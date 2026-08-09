#include <cassert>
#include <filesystem>
#include <string>

#include "agent/approval/store.hpp"
#include "agent/internal/platform_io.hpp"

namespace {
using namespace agent_framework;

approval::ApprovalRequest request(std::string id = "approval-a",
                                  std::string tenant = "tenant-a") {
    approval::ApprovalRequest value;
    value.metadata.identity.tenant_id = std::move(tenant);
    value.metadata.identity.task_id = "task-a";
    value.metadata.identity.run_id = "run-a";
    value.approval_id = std::move(id);
    value.request_kind = "plan";
    value.requester_id = "requester-a";
    value.scope = "plan:plan-a";
    value.reason = "medium-risk side effect";
    value.risk_level = "medium";
    value.policy_revision = "policy-v1";
    value.plan_digest = "sha256:plan";
    value.arguments_digest = "sha256:arguments";
    value.artifact_digest = "sha256:artifact";
    value.memory_view_digest = "sha256:view";
    value.created_at = "2026-08-09T00:00:00Z";
    value.expires_at = "2026-08-10T00:00:00Z";
    return value;
}

approval::ApprovalDecision decision(const approval::ApprovalRequest& request_value,
                                    approval::Decision value = approval::Decision::Approved) {
    approval::ApprovalDecision result;
    result.metadata = request_value.metadata;
    result.approval_id = request_value.approval_id;
    result.request_digest = approval::encode(request_value).at("canonical_digest");
    result.reviewer_id = "reviewer-a";
    result.decision = value;
    result.scope = request_value.scope;
    result.reason = "reviewed evidence and constraints";
    result.policy_revision = request_value.policy_revision;
    result.plan_digest = request_value.plan_digest;
    result.arguments_digest = request_value.arguments_digest;
    result.artifact_digest = request_value.artifact_digest;
    result.memory_view_digest = request_value.memory_view_digest;
    result.decided_at = "2026-08-09T01:00:00Z";
    result.expires_at = request_value.expires_at;
    return result;
}
}  // namespace

int main() {
    using namespace agent_framework;
    const auto root = std::filesystem::temp_directory_path() /
        ("agent-phase4-approval-" + std::to_string(internal::current_process_id()));
    const auto path = root / "approval.sqlite3";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    const auto approval_request = request();
    {
        approval::SQLiteApprovalStore store(path.string());
        assert(store.put_request(approval_request));
        assert(store.put_request(approval_request).status ==
               approval::ApprovalStoreStatus::Duplicate);
        assert(store.pending("tenant-a", "2026-08-09T00:30:00Z", 10).size() == 1);
        assert(store.pending("tenant-b", "2026-08-09T00:30:00Z", 10).empty());

        auto self_approval = decision(approval_request);
        self_approval.reviewer_id = approval_request.requester_id;
        assert(store.decide(self_approval, 0).status ==
               approval::ApprovalStoreStatus::Invalid);
        auto tampered = decision(approval_request);
        tampered.plan_digest = "sha256:different-plan";
        assert(store.decide(tampered, 0).status == approval::ApprovalStoreStatus::Invalid);

        const auto approved = store.decide(decision(approval_request), 0);
        assert(approved && approved.revision == 1);
        assert(store.pending("tenant-a", "2026-08-09T00:30:00Z", 10).empty());
        assert(store.decide(decision(approval_request), 0).status ==
               approval::ApprovalStoreStatus::RevisionConflict);

        auto revoked = decision(approval_request, approval::Decision::Revoked);
        revoked.decided_at = "2026-08-09T02:00:00Z";
        assert(store.decide(revoked, 1).revision == 2);
        assert(store.latest_decision("approval-a")->decision == approval::Decision::Revoked);
        assert(store.decision_history("approval-a").size() == 2);

        auto expired_request = request("approval-expired");
        expired_request.expires_at = "2026-08-08T00:00:00Z";
        assert(store.put_request(expired_request));
        assert(store.pending("tenant-a", "2026-08-09T00:00:00Z", 10).empty());
        auto too_late = decision(expired_request);
        too_late.decided_at = "2026-08-09T00:00:00Z";
        assert(store.decide(too_late, 0).status == approval::ApprovalStoreStatus::Invalid);
    }

    {
        approval::SQLiteApprovalStore recovered(path.string());
        assert(recovered.request("approval-a"));
        assert(recovered.latest_decision("approval-a")->decision == approval::Decision::Revoked);
        assert(recovered.decision_history("approval-a").size() == 2);
    }

#if !defined(_WIN32)
    const auto permissions = std::filesystem::status(path).permissions();
    assert((permissions & std::filesystem::perms::group_all) == std::filesystem::perms::none);
    assert((permissions & std::filesystem::perms::others_all) == std::filesystem::perms::none);
#endif
    std::filesystem::remove_all(root, error);
    return 0;
}
