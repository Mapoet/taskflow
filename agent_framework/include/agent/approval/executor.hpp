#pragma once

#include <mutex>
#include <set>
#include <string>

#include "agent/approval/policy.hpp"
#include "agent/approval/store.hpp"
#include "agent/harness/runtime.hpp"

namespace agent_framework::approval {

struct ReviewerIdentity {
    std::string principal_id;
    std::set<std::string> roles;
    std::string delegated_by;
    std::set<std::string> delegated_scopes;
};

struct ApprovalReview {
    std::string approval_id;
    std::string request_digest;
    ReviewerIdentity reviewer;
    Decision decision{Decision::Rejected};
    std::string reason;
    std::string decided_at;
};

enum class ReviewOutcome {
    AwaitingAdditionalReview,
    Approved,
    Rejected,
    Expired,
    Denied,
    RevisionConflict,
    Error
};

struct ReviewResult {
    ReviewOutcome outcome{ReviewOutcome::Error};
    std::uint64_t vote_revision{0};
    std::uint64_t decision_revision{0};
    std::string decision_digest;
    std::string error_code;
    std::string error_message;
};

struct RevisionResult {
    bool committed{false};
    std::string request_digest;
    std::string error_code;
};

class AccountableApprovalExecutor {
public:
    AccountableApprovalExecutor(std::string journal_path, ApprovalStore& store,
                                PolicyDecisionPoint policy = PolicyDecisionPoint{});
    ~AccountableApprovalExecutor();
    AccountableApprovalExecutor(const AccountableApprovalExecutor&) = delete;
    AccountableApprovalExecutor& operator=(const AccountableApprovalExecutor&) = delete;

    ReviewResult review(const ApprovalReview& review, std::uint64_t expected_vote_revision);
    ReviewResult reconcile(std::string_view approval_id, std::string_view now);
    RevisionResult revise(std::string_view original_approval_id,
                          const ApprovalRequest& revised,
                          const ReviewerIdentity& editor,
                          std::string_view original_request_digest);
    std::uint64_t vote_revision(std::string_view approval_id);

private:
    void* db_{nullptr};
    ApprovalStore& store_;
    PolicyDecisionPoint policy_;
    std::mutex mutex_;
};

class ApprovalHarnessPort final : public harness::HarnessStagePort {
public:
    ApprovalHarnessPort(std::string port_id, ApprovalStore& store,
                        std::string approval_id, std::string now);
    std::string id() const override { return id_; }
    bool may_have_side_effects() const noexcept override { return false; }
    harness::HarnessStageResult execute(const harness::HarnessStageRequest& request) override;
private:
    std::string id_;
    ApprovalStore& store_;
    std::string approval_id_;
    std::string now_;
};

}  // namespace agent_framework::approval
