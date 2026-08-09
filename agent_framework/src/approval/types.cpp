#include "agent/approval/types.hpp"

#include <set>

namespace agent_framework::approval {
namespace {
using json = nlohmann::json;
std::string decision_string(Decision value) {
    switch(value) {
        case Decision::Approved: return "approved";
        case Decision::Rejected: return "rejected";
        case Decision::Edited: return "edited";
        case Decision::Expired: return "expired";
        case Decision::Revoked: return "revoked";
    }
    return "rejected";
}
Decision decision_value(const std::string& value) {
    if(value == "approved") return Decision::Approved;
    if(value == "rejected") return Decision::Rejected;
    if(value == "edited") return Decision::Edited;
    if(value == "expired") return Decision::Expired;
    if(value == "revoked") return Decision::Revoked;
    throw std::invalid_argument("unknown approval decision");
}
}  // namespace
json encode(const ApprovalRequest& value) {
    return contracts::make_typed_contract(value.metadata, "agent.approval_request/v1",
        {{"approval_id", value.approval_id}, {"request_kind", value.request_kind},
         {"requester_id", value.requester_id}, {"scope", value.scope}, {"reason", value.reason},
         {"risk_level", value.risk_level}, {"policy_revision", value.policy_revision},
         {"plan_digest", value.plan_digest}, {"arguments_digest", value.arguments_digest},
         {"artifact_digest", value.artifact_digest}, {"memory_view_digest", value.memory_view_digest},
         {"proposed_change", value.proposed_change}, {"created_at", value.created_at},
         {"expires_at", value.expires_at}});
}
json encode(const ApprovalDecision& value) {
    return contracts::make_typed_contract(value.metadata, "agent.approval_decision/v1",
        {{"approval_id", value.approval_id}, {"request_digest", value.request_digest},
         {"reviewer_id", value.reviewer_id}, {"decision", decision_string(value.decision)},
         {"scope", value.scope}, {"reason", value.reason},
         {"policy_revision", value.policy_revision}, {"plan_digest", value.plan_digest},
         {"arguments_digest", value.arguments_digest}, {"artifact_digest", value.artifact_digest},
         {"memory_view_digest", value.memory_view_digest}, {"decided_at", value.decided_at},
         {"expires_at", value.expires_at}});
}
std::optional<ApprovalRequest> decode_approval_request(
    const json& value, const contracts::ParseContext& context,
    std::vector<contracts::ContractIssue>* issues) {
    static const std::set<std::string> fields = {"approval_id", "request_kind", "requester_id",
        "scope", "reason", "risk_level", "policy_revision", "plan_digest", "arguments_digest",
        "artifact_digest", "memory_view_digest", "proposed_change", "created_at", "expires_at"};
    auto document = contracts::parse_typed_contract(value, "agent.approval_request/v1", context, issues);
    if(!document || !contracts::validate_object_fields(document->payload, fields, fields,
        contracts::UnknownFieldPolicy::Reject, nullptr, issues, "/payload")) return std::nullopt;
    try {
        const auto& p = document->payload;
        ApprovalRequest result;
        result.metadata = std::move(document->metadata);
        result.approval_id=p.at("approval_id").get<std::string>(); result.request_kind=p.at("request_kind").get<std::string>();
        result.requester_id=p.at("requester_id").get<std::string>(); result.scope=p.at("scope").get<std::string>();
        result.reason=p.at("reason").get<std::string>(); result.risk_level=p.at("risk_level").get<std::string>();
        result.policy_revision=p.at("policy_revision").get<std::string>(); result.plan_digest=p.at("plan_digest").get<std::string>();
        result.arguments_digest=p.at("arguments_digest").get<std::string>(); result.artifact_digest=p.at("artifact_digest").get<std::string>();
        result.memory_view_digest=p.at("memory_view_digest").get<std::string>(); result.proposed_change=p.at("proposed_change");
        result.created_at=p.at("created_at").get<std::string>(); result.expires_at=p.at("expires_at").get<std::string>();
        return result;
    } catch(const std::exception& error) {
        contracts::append_issue(issues, "payload_decode_failed", "/payload", error.what());
        return std::nullopt;
    }
}
std::optional<ApprovalDecision> decode_approval_decision(
    const json& value, const contracts::ParseContext& context,
    std::vector<contracts::ContractIssue>* issues) {
    static const std::set<std::string> fields = {"approval_id", "request_digest", "reviewer_id",
        "decision", "scope", "reason", "policy_revision", "plan_digest", "arguments_digest",
        "artifact_digest", "memory_view_digest", "decided_at", "expires_at"};
    auto document = contracts::parse_typed_contract(value, "agent.approval_decision/v1",
                                                     context, issues);
    if(!document || !contracts::validate_object_fields(document->payload, fields, fields,
        contracts::UnknownFieldPolicy::Reject, nullptr, issues, "/payload")) return std::nullopt;
    try {
        const auto& p = document->payload;
        ApprovalDecision result;
        result.metadata = std::move(document->metadata);
        result.approval_id = p.at("approval_id").get<std::string>();
        result.request_digest = p.at("request_digest").get<std::string>();
        result.reviewer_id = p.at("reviewer_id").get<std::string>();
        result.decision = decision_value(p.at("decision").get<std::string>());
        result.scope = p.at("scope").get<std::string>();
        result.reason = p.at("reason").get<std::string>();
        result.policy_revision = p.at("policy_revision").get<std::string>();
        result.plan_digest = p.at("plan_digest").get<std::string>();
        result.arguments_digest = p.at("arguments_digest").get<std::string>();
        result.artifact_digest = p.at("artifact_digest").get<std::string>();
        result.memory_view_digest = p.at("memory_view_digest").get<std::string>();
        result.decided_at = p.at("decided_at").get<std::string>();
        result.expires_at = p.at("expires_at").get<std::string>();
        return result;
    } catch(const std::exception& error) {
        contracts::append_issue(issues, "payload_decode_failed", "/payload", error.what());
        return std::nullopt;
    }
}
}  // namespace agent_framework::approval
