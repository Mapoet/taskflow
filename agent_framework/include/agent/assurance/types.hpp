#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "agent/contracts/contract.hpp"

namespace agent_framework::assurance {

enum class VerificationLayer { Functional, Module, Integration, System, Metric };
enum class FindingOutcome { Pass, Fail, Partial, Inconclusive };
enum class AcceptanceDecision { Accepted, Rejected, Partial, ManualReview };

struct Criterion {
    std::string criterion_id;
    VerificationLayer layer{VerificationLayer::Functional};
    std::string claim;
    std::string oracle_kind;
    std::vector<std::string> required_evidence;
    std::string threshold;
    bool mandatory{true};
};

struct AcceptanceContract {
    contracts::ContractMetadata metadata;
    std::uint64_t revision{1};
    std::string plan_digest;
    std::vector<Criterion> criteria;
};

struct Finding {
    std::string finding_id;
    std::string criterion_id;
    std::string severity;
    FindingOutcome outcome{FindingOutcome::Inconclusive};
    double confidence{0.0};
    std::vector<std::string> evidence_ids;
    std::string remediation;
};

struct AcceptanceReport {
    contracts::ContractMetadata metadata;
    std::string plan_digest;
    std::string acceptance_contract_digest;
    std::string artifact_manifest_digest;
    std::string memory_snapshot_id;
    std::string verification_view_digest;
    std::vector<Finding> findings;
    std::vector<std::string> residual_risks;
    AcceptanceDecision decision{AcceptanceDecision::ManualReview};
};

nlohmann::json encode(const AcceptanceContract& value);
nlohmann::json encode(const AcceptanceReport& value);
std::optional<AcceptanceContract> decode_acceptance_contract(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<AcceptanceReport> decode_acceptance_report(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);

}  // namespace agent_framework::assurance
