#pragma once

#include <optional>
#include <string>

#include "agent/contracts/contract.hpp"

namespace agent_framework::approval {

enum class Decision { Approved, Rejected, Edited, Expired, Revoked };

struct ApprovalRequest {
    contracts::ContractMetadata metadata;
    std::string approval_id;
    std::string request_kind;
    std::string requester_id;
    std::string scope;
    std::string reason;
    std::string risk_level;
    std::string policy_revision;
    std::string plan_digest;
    std::string arguments_digest;
    std::string artifact_digest;
    std::string memory_view_digest;
    nlohmann::json proposed_change = nlohmann::json::object();
    std::string created_at;
    std::string expires_at;
};

struct ApprovalDecision {
    contracts::ContractMetadata metadata;
    std::string approval_id;
    std::string request_digest;
    std::string reviewer_id;
    Decision decision{Decision::Rejected};
    std::string scope;
    std::string reason;
    std::string policy_revision;
    std::string plan_digest;
    std::string arguments_digest;
    std::string artifact_digest;
    std::string memory_view_digest;
    std::string decided_at;
    std::string expires_at;
};

nlohmann::json encode(const ApprovalRequest& value);
nlohmann::json encode(const ApprovalDecision& value);
std::optional<ApprovalRequest> decode_approval_request(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<ApprovalDecision> decode_approval_decision(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);

}  // namespace agent_framework::approval
