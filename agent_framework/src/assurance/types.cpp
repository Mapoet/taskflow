#include "agent/assurance/types.hpp"

#include <set>

namespace agent_framework::assurance {
namespace {
using json = nlohmann::json;
using contracts::ContractIssue;

std::string layer_string(VerificationLayer value) {
    switch(value) {
        case VerificationLayer::Functional: return "functional";
        case VerificationLayer::Module: return "module";
        case VerificationLayer::Integration: return "integration";
        case VerificationLayer::System: return "system";
        case VerificationLayer::Metric: return "metric";
    }
    return "functional";
}
VerificationLayer layer_value(const std::string& value) {
    if(value == "functional") return VerificationLayer::Functional;
    if(value == "module") return VerificationLayer::Module;
    if(value == "integration") return VerificationLayer::Integration;
    if(value == "system") return VerificationLayer::System;
    if(value == "metric") return VerificationLayer::Metric;
    throw std::invalid_argument("unknown verification layer");
}
std::string outcome_string(FindingOutcome value) {
    switch(value) {
        case FindingOutcome::Pass: return "pass";
        case FindingOutcome::Fail: return "fail";
        case FindingOutcome::Partial: return "partial";
        case FindingOutcome::Inconclusive: return "inconclusive";
    }
    return "inconclusive";
}
FindingOutcome outcome_value(const std::string& value) {
    if(value == "pass") return FindingOutcome::Pass;
    if(value == "fail") return FindingOutcome::Fail;
    if(value == "partial") return FindingOutcome::Partial;
    if(value == "inconclusive") return FindingOutcome::Inconclusive;
    throw std::invalid_argument("unknown finding outcome");
}
std::string decision_string(AcceptanceDecision value) {
    switch(value) {
        case AcceptanceDecision::Accepted: return "accepted";
        case AcceptanceDecision::Rejected: return "rejected";
        case AcceptanceDecision::Partial: return "partial";
        case AcceptanceDecision::ManualReview: return "manual_review";
    }
    return "manual_review";
}
AcceptanceDecision decision_value(const std::string& value) {
    if(value == "accepted") return AcceptanceDecision::Accepted;
    if(value == "rejected") return AcceptanceDecision::Rejected;
    if(value == "partial") return AcceptanceDecision::Partial;
    if(value == "manual_review") return AcceptanceDecision::ManualReview;
    throw std::invalid_argument("unknown acceptance decision");
}
json criterion_json(const Criterion& value) {
    return {{"criterion_id", value.criterion_id}, {"layer", layer_string(value.layer)},
            {"claim", value.claim}, {"oracle_kind", value.oracle_kind},
            {"required_evidence", value.required_evidence}, {"threshold", value.threshold},
            {"mandatory", value.mandatory}};
}
Criterion criterion_value(const json& value) {
    return {value.at("criterion_id").get<std::string>(),
            layer_value(value.at("layer").get<std::string>()),
            value.at("claim").get<std::string>(), value.at("oracle_kind").get<std::string>(),
            value.at("required_evidence").get<std::vector<std::string>>(),
            value.at("threshold").get<std::string>(), value.at("mandatory").get<bool>()};
}
json finding_json(const Finding& value) {
    return {{"finding_id", value.finding_id}, {"criterion_id", value.criterion_id},
            {"severity", value.severity}, {"outcome", outcome_string(value.outcome)},
            {"confidence", value.confidence}, {"evidence_ids", value.evidence_ids},
            {"remediation", value.remediation}};
}
Finding finding_value(const json& value) {
    return {value.at("finding_id").get<std::string>(),
            value.at("criterion_id").get<std::string>(), value.at("severity").get<std::string>(),
            outcome_value(value.at("outcome").get<std::string>()),
            value.at("confidence").get<double>(),
            value.at("evidence_ids").get<std::vector<std::string>>(),
            value.at("remediation").get<std::string>()};
}
template <typename T, typename Builder>
std::optional<T> decode_value(const json& value, const char* kind,
                              const std::set<std::string>& fields,
                              const contracts::ParseContext& context,
                              std::vector<ContractIssue>* issues, Builder builder) {
    auto document = contracts::parse_typed_contract(value, kind, context, issues);
    if(!document || !contracts::validate_object_fields(document->payload, fields, fields,
        contracts::UnknownFieldPolicy::Reject, nullptr, issues, "/payload")) return std::nullopt;
    try {
        T result = builder(document->payload);
        result.metadata = std::move(document->metadata);
        return result;
    } catch(const std::exception& error) {
        contracts::append_issue(issues, "payload_decode_failed", "/payload", error.what());
        return std::nullopt;
    }
}
}  // namespace

json encode(const AcceptanceContract& value) {
    json criteria = json::array();
    for(const auto& item : value.criteria) criteria.push_back(criterion_json(item));
    return contracts::make_typed_contract(value.metadata, "agent.acceptance_contract/v1",
        {{"revision", value.revision}, {"plan_digest", value.plan_digest},
         {"criteria", std::move(criteria)}});
}
json encode(const AcceptanceReport& value) {
    json findings = json::array();
    for(const auto& item : value.findings) findings.push_back(finding_json(item));
    return contracts::make_typed_contract(value.metadata, "agent.acceptance_report/v1",
        {{"plan_digest", value.plan_digest},
         {"acceptance_contract_digest", value.acceptance_contract_digest},
         {"artifact_manifest_digest", value.artifact_manifest_digest},
         {"memory_snapshot_id", value.memory_snapshot_id},
         {"verification_view_digest", value.verification_view_digest},
         {"findings", std::move(findings)}, {"residual_risks", value.residual_risks},
         {"decision", decision_string(value.decision)}});
}
std::optional<AcceptanceContract> decode_acceptance_contract(
    const json& value, const contracts::ParseContext& context,
    std::vector<ContractIssue>* issues) {
    static const std::set<std::string> fields = {"revision", "plan_digest", "criteria"};
    return decode_value<AcceptanceContract>(value, "agent.acceptance_contract/v1", fields,
        context, issues, [](const json& p) {
            AcceptanceContract result;
            result.revision = p.at("revision").get<std::uint64_t>();
            result.plan_digest = p.at("plan_digest").get<std::string>();
            for(const auto& item : p.at("criteria")) result.criteria.push_back(criterion_value(item));
            return result;
        });
}
std::optional<AcceptanceReport> decode_acceptance_report(
    const json& value, const contracts::ParseContext& context,
    std::vector<ContractIssue>* issues) {
    static const std::set<std::string> fields = {
        "plan_digest", "acceptance_contract_digest", "artifact_manifest_digest",
        "memory_snapshot_id", "verification_view_digest", "findings", "residual_risks", "decision"};
    return decode_value<AcceptanceReport>(value, "agent.acceptance_report/v1", fields,
        context, issues, [](const json& p) {
            AcceptanceReport result;
            result.plan_digest = p.at("plan_digest").get<std::string>();
            result.acceptance_contract_digest = p.at("acceptance_contract_digest").get<std::string>();
            result.artifact_manifest_digest = p.at("artifact_manifest_digest").get<std::string>();
            result.memory_snapshot_id = p.at("memory_snapshot_id").get<std::string>();
            result.verification_view_digest = p.at("verification_view_digest").get<std::string>();
            for(const auto& item : p.at("findings")) result.findings.push_back(finding_value(item));
            result.residual_risks = p.at("residual_risks").get<std::vector<std::string>>();
            result.decision = decision_value(p.at("decision").get<std::string>());
            return result;
        });
}
}  // namespace agent_framework::assurance
