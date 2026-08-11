#include <cassert>
#include <filesystem>
#include <memory>

#include "agent/approval/executor.hpp"
#include "agent/internal/platform_io.hpp"
#include "phase4_harness_test_support.hpp"

namespace {
using namespace agent_framework;
approval::ApprovalRequest request(std::string id, std::string risk = "high") {
    approval::ApprovalRequest out;
    out.metadata = phase4_harness_test::metadata();
    out.approval_id = std::move(id);
    out.request_kind = "plan";
    out.requester_id = "requester-a";
    out.scope = "plan:plan-a";
    out.reason = "governed execution";
    out.risk_level = std::move(risk);
    out.policy_revision = "phase4-policy-v1";
    out.plan_digest = "sha256:plan-v1";
    out.arguments_digest = "sha256:arguments";
    out.artifact_digest = "sha256:artifact";
    out.memory_view_digest = "sha256:view";
    out.created_at = "2026-08-11T00:00:00Z";
    out.expires_at = "2026-08-12T00:00:00Z";
    return out;
}
approval::ApprovalReview review(const approval::ApprovalRequest& request,
                                std::string reviewer, std::string time = "2026-08-11T01:00:00Z") {
    approval::ApprovalReview out;
    out.approval_id = request.approval_id;
    out.request_digest = approval::encode(request).at("canonical_digest");
    out.reviewer.principal_id = std::move(reviewer);
    out.reviewer.roles = {"approver"};
    out.decision = approval::Decision::Approved;
    out.reason = "independent evidence reviewed";
    out.decided_at = std::move(time);
    return out;
}
}

int main() {
    using namespace agent_framework;
    using namespace agent_framework::harness;
    using namespace phase4_harness_test;
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("phase4-approval-executor-" + std::to_string(internal::current_process_id()));
    std::error_code ec; fs::remove_all(root, ec); fs::create_directories(root);
    approval::SQLiteApprovalStore store((root / "approval.sqlite3").string());
    const auto high = request("approval-high");
    assert(store.put_request(high));
    {
        approval::AccountableApprovalExecutor executor((root / "votes.sqlite3").string(), store);
        auto self = review(high, high.requester_id);
        assert(executor.review(self, 0).error_code == "separation_of_duties_violation");
        auto unauthorized = review(high, "viewer-a"); unauthorized.reviewer.roles = {"viewer"};
        assert(executor.review(unauthorized, 0).error_code == "reviewer_not_authorized");
        auto delegated = review(high, "delegate-a"); delegated.reviewer.delegated_by = "approver-root";
        assert(executor.review(delegated, 0).error_code == "delegation_grant_required");
        auto first = executor.review(review(high, "reviewer-a"), 0);
        assert(first.outcome == approval::ReviewOutcome::AwaitingAdditionalReview);
        assert(!store.latest_decision(high.approval_id));
        assert(executor.review(review(high, "reviewer-c"), 0).outcome ==
               approval::ReviewOutcome::RevisionConflict);
    }
    {
        approval::AccountableApprovalExecutor recovered((root / "votes.sqlite3").string(), store);
        assert(recovered.vote_revision(high.approval_id) == 1);
        auto duplicate = recovered.review(review(high, "reviewer-a"), 1);
        assert(duplicate.error_code == "duplicate_or_invalid_reviewer");
        auto second = recovered.review(review(high, "reviewer-b"), 1);
        assert(second.outcome == approval::ReviewOutcome::Approved);
        assert(second.vote_revision == 2 && second.decision_revision == 1);
        assert(store.latest_decision(high.approval_id)->decision == approval::Decision::Approved);
    }

    const auto expired = request("approval-expired", "medium");
    assert(store.put_request(expired));
    approval::AccountableApprovalExecutor executor((root / "votes.sqlite3").string(), store);
    approval::AuthenticatedApprovalActionService action_service(executor);
    auto unauthenticated = action_service.submit({}, {expired.approval_id,
        approval::encode(expired).at("canonical_digest"), "approve", "test",
        "2026-08-11T01:00:00Z", 0});
    assert(unauthenticated.error_code == "authenticated_identity_required");
    assert(executor.review(review(expired, "reviewer-a", "2026-08-13T00:00:00Z"), 0).outcome ==
           approval::ReviewOutcome::Expired);

    const auto delegated_request = request("approval-delegated", "medium");
    assert(store.put_request(delegated_request));
    auto delegated_review = review(delegated_request, "delegate-a");
    delegated_review.reviewer.delegated_by = "approver-root";
    delegated_review.reviewer.delegated_scopes = {delegated_request.scope};
    delegated_review.reviewer.delegation_grant_id = "grant-a";
    assert(executor.grant_delegation({"grant-a", "approver-root", "delegate-a",
        {delegated_request.scope}, {"approver"}, "high", "2026-08-10T00:00:00Z",
        "2026-08-12T00:00:00Z", 1, "sha256:authority"}));
    assert(executor.review(delegated_review, 0).outcome == approval::ReviewOutcome::Approved);
    assert(executor.reconcile(delegated_request.approval_id, "2026-08-11T02:00:00Z").outcome ==
           approval::ReviewOutcome::Approved);

    const auto revoked_request = request("approval-delegation-revoked", "medium");
    assert(store.put_request(revoked_request));
    assert(executor.grant_delegation({"grant-revoked", "approver-root", "delegate-b",
        {revoked_request.scope}, {"approver"}, "medium", "2026-08-10T00:00:00Z",
        "2026-08-12T00:00:00Z", 1, "sha256:authority"}));
    assert(executor.revoke_delegation("grant-revoked", "2026-08-11T00:30:00Z"));
    auto revoked_review = review(revoked_request, "delegate-b");
    revoked_review.reviewer.delegated_by = "approver-root";
    revoked_review.reviewer.delegation_grant_id = "grant-revoked";
    assert(executor.review(revoked_review, 0).error_code == "delegation_scope_denied");

    auto role_quorum = request("approval-role-quorum", "medium");
    role_quorum.proposed_change = {{"required_quorum", 2},
                                   {"required_roles", {"security", "release"}}};
    assert(store.put_request(role_quorum));
    auto security_vote = review(role_quorum, "security-a");
    security_vote.reviewer.roles = {"approver", "security"};
    assert(executor.review(security_vote, 0).outcome ==
           approval::ReviewOutcome::AwaitingAdditionalReview);
    auto release_vote = review(role_quorum, "release-a");
    release_vote.reviewer.roles = {"approver", "release"};
    assert(executor.review(release_vote, 1).outcome == approval::ReviewOutcome::Approved);

    auto revised = request("approval-high-r2");
    revised.plan_digest = "sha256:plan-v2";
    revised.proposed_change = {{"kind", "plan_edit"}, {"parent_approval_id", high.approval_id}};
    approval::ReviewerIdentity editor{"editor-a", {"approver"}, {}, {}};
    const auto revision = executor.revise(high.approval_id, revised, editor,
        approval::encode(high).at("canonical_digest").get<std::string>());
    assert(revision.committed);
    assert(store.request(revised.approval_id)->plan_digest == "sha256:plan-v2");
    assert(executor.superseded(high.approval_id));
    assert(executor.review(review(high, "reviewer-after-edit"), 2).error_code == "approval_superseded");

    const auto escalation_request = request("approval-escalation", "medium");
    assert(store.put_request(escalation_request));
    assert(executor.escalate({"escalation-a", escalation_request.approval_id,
        "release-managers", "deadline approaching", "2026-08-11T02:00:00Z"}));
    assert(executor.escalation(escalation_request.approval_id)->target_group == "release-managers");

    const auto harness_request = request("approval-harness", "medium");
    assert(store.put_request(harness_request));
    InMemoryHarnessStore harness_store;
    HarnessPortRegistry registry;
    for(std::size_t index = 0; index < static_cast<std::size_t>(HarnessStage::Complete); ++index) {
        const auto stage = static_cast<HarnessStage>(index);
        std::shared_ptr<HarnessStagePort> port;
        if(stage == HarnessStage::PlanApproval)
            port = std::make_shared<approval::ApprovalHarnessPort>(
                "approval-store-port", store, harness_request.approval_id, "2026-08-11T02:00:00Z");
        else
            port = std::make_shared<CallbackHarnessStagePort>("port-" + harness_stage_name(stage),
                side_effecting(stage), [](const HarnessStageRequest& request) {
                    return successful(request, false);
                }, [](const HarnessStageRequest& request) -> std::optional<HarnessStageResult> {
                    return successful(request, false);
                });
        assert(registry.bind(stage, std::move(port)));
    }
    Phase4HarnessRuntime runtime(harness_store, std::move(registry));
    auto waiting = runtime.run(start("harness-approval-store"));
    assert(waiting.state == HarnessState::AwaitingApproval);
    auto approved = executor.review(review(harness_request, "reviewer-z"), 0);
    assert(approved.outcome == approval::ReviewOutcome::Approved);
    auto resumed = runtime.resume("tenant-a", "harness-approval-store");
    assert(resumed.state == HarnessState::Completed);
    assert(resumed.checkpoint.pins.approval_decision_id.find("approval-harness:") == 0);
    fs::remove_all(root, ec);
    return 0;
}
