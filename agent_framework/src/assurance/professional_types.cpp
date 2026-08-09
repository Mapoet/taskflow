#include "agent/assurance/professional_workflow.hpp"

#include <set>
#include <stdexcept>

namespace agent_framework::assurance {
namespace {

using json = nlohmann::json;
using contracts::ContractIssue;

std::string outcome_name(FindingOutcome value) {
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

std::string strength_name(OracleStrength value) {
    switch(value) {
        case OracleStrength::Deterministic: return "deterministic";
        case OracleStrength::RealSystem: return "real_system";
        case OracleStrength::Authoritative: return "authoritative";
        case OracleStrength::StaticAnalysis: return "static_analysis";
        case OracleStrength::CalibratedModel: return "calibrated_model";
        case OracleStrength::UncalibratedClaim: return "uncalibrated_claim";
    }
    return "uncalibrated_claim";
}

OracleStrength strength_value(const std::string& value) {
    if(value == "deterministic") return OracleStrength::Deterministic;
    if(value == "real_system") return OracleStrength::RealSystem;
    if(value == "authoritative") return OracleStrength::Authoritative;
    if(value == "static_analysis") return OracleStrength::StaticAnalysis;
    if(value == "calibrated_model") return OracleStrength::CalibratedModel;
    if(value == "uncalibrated_claim") return OracleStrength::UncalibratedClaim;
    throw std::invalid_argument("unknown oracle strength");
}

json evidence_json(const VerificationEvidence& value) {
    return {{"evidence_id", value.evidence_id}, {"criterion_id", value.criterion_id},
            {"source_kind", value.source_kind}, {"source_locator", value.source_locator},
            {"content_digest", value.content_digest}, {"observed_at", value.observed_at},
            {"freshness_deadline", value.freshness_deadline},
            {"oracle_strength", strength_name(value.oracle_strength)},
            {"outcome", outcome_name(value.outcome)}, {"independent", value.independent}};
}

VerificationEvidence evidence_value(const json& value) {
    static const std::set<std::string> fields = {
        "evidence_id", "criterion_id", "source_kind", "source_locator", "content_digest",
        "observed_at", "freshness_deadline", "oracle_strength", "outcome", "independent"};
    if(!contracts::validate_object_fields(value, fields, fields,
        contracts::UnknownFieldPolicy::Reject, nullptr, nullptr, "/evidence"))
        throw std::invalid_argument("invalid verification evidence fields");
    auto result = VerificationEvidence{value.at("evidence_id").get<std::string>(),
            value.at("criterion_id").get<std::string>(),
            value.at("source_kind").get<std::string>(),
            value.at("source_locator").get<std::string>(),
            value.at("content_digest").get<std::string>(),
            value.at("observed_at").get<std::string>(),
            value.at("freshness_deadline").get<std::string>(),
            strength_value(value.at("oracle_strength").get<std::string>()),
            outcome_value(value.at("outcome").get<std::string>()),
            value.at("independent").get<bool>()};
    if(result.evidence_id.empty() || result.criterion_id.empty() ||
       result.source_locator.empty() || result.content_digest.rfind("sha256:", 0) != 0)
        throw std::invalid_argument("invalid verification evidence identity or digest");
    return result;
}

json finding_json(const Finding& value) {
    return {{"finding_id", value.finding_id}, {"criterion_id", value.criterion_id},
            {"severity", value.severity}, {"outcome", outcome_name(value.outcome)},
            {"confidence", value.confidence}, {"evidence_ids", value.evidence_ids},
            {"remediation", value.remediation}};
}

Finding finding_value(const json& value) {
    static const std::set<std::string> fields = {
        "finding_id", "criterion_id", "severity", "outcome", "confidence",
        "evidence_ids", "remediation"};
    if(!contracts::validate_object_fields(value, fields, fields,
        contracts::UnknownFieldPolicy::Reject, nullptr, nullptr, "/finding"))
        throw std::invalid_argument("invalid finding fields");
    auto result = Finding{value.at("finding_id").get<std::string>(),
            value.at("criterion_id").get<std::string>(),
            value.at("severity").get<std::string>(),
            outcome_value(value.at("outcome").get<std::string>()),
            value.at("confidence").get<double>(),
            value.at("evidence_ids").get<std::vector<std::string>>(),
            value.at("remediation").get<std::string>()};
    if(result.finding_id.empty() || result.criterion_id.empty() ||
       result.confidence < 0.0 || result.confidence > 1.0)
        throw std::invalid_argument("invalid finding identity or confidence");
    return result;
}

json assignment_json(const VerificationAssignment& value) {
    return {{"assignment_id", value.assignment_id},
            {"role", professional_role_name(value.role)},
            {"criterion_ids", value.criterion_ids},
            {"required_evidence", value.required_evidence},
            {"granted_capabilities", value.granted_capabilities},
            {"read_only", value.read_only}, {"mandatory", value.mandatory},
            {"rationale", value.rationale}};
}

VerificationAssignment assignment_value(const json& value) {
    static const std::set<std::string> fields = {
        "assignment_id", "role", "criterion_ids", "required_evidence",
        "granted_capabilities", "read_only", "mandatory", "rationale"};
    if(!contracts::validate_object_fields(value, fields, fields,
        contracts::UnknownFieldPolicy::Reject, nullptr, nullptr, "/assignment"))
        throw std::invalid_argument("invalid verification assignment fields");
    const auto role = professional_role_from_name(value.at("role").get<std::string>());
    if(!role) throw std::invalid_argument("invalid professional role");
    return {value.at("assignment_id").get<std::string>(), *role,
            value.at("criterion_ids").get<std::vector<std::string>>(),
            value.at("required_evidence").get<std::vector<std::string>>(),
            value.at("granted_capabilities").get<std::vector<std::string>>(),
            value.at("read_only").get<bool>(), value.at("mandatory").get<bool>(),
            value.at("rationale").get<std::string>()};
}

json plan_payload(const VerificationPlan& value) {
    json assignments = json::array();
    for(const auto& item : value.assignments) assignments.push_back(assignment_json(item));
    return {{"verification_plan_id", value.verification_plan_id},
            {"revision", value.revision},
            {"acceptance_contract_digest", value.acceptance_contract_digest},
            {"task_plan_digest", value.task_plan_digest},
            {"artifact_manifest_digest", value.artifact_manifest_digest},
            {"assignments", std::move(assignments)},
            {"planner_invocation_id", value.planner_invocation_id},
            {"created_at", value.created_at}};
}

VerificationPlan plan_from_payload(const json& payload) {
    VerificationPlan result;
    result.verification_plan_id = payload.at("verification_plan_id").get<std::string>();
    result.revision = payload.at("revision").get<std::uint64_t>();
    result.acceptance_contract_digest = payload.at("acceptance_contract_digest").get<std::string>();
    result.task_plan_digest = payload.at("task_plan_digest").get<std::string>();
    result.artifact_manifest_digest = payload.at("artifact_manifest_digest").get<std::string>();
    for(const auto& item : payload.at("assignments")) result.assignments.push_back(assignment_value(item));
    result.planner_invocation_id = payload.at("planner_invocation_id").get<std::string>();
    result.created_at = payload.at("created_at").get<std::string>();
    if(result.verification_plan_id.empty() || result.revision == 0 ||
       result.acceptance_contract_digest.empty() || result.task_plan_digest.empty() ||
       result.artifact_manifest_digest.empty() || result.assignments.empty() ||
       result.planner_invocation_id.empty())
        throw std::invalid_argument("verification plan bindings and assignments are required");
    return result;
}

json conflict_json(const EvidenceConflict& value) {
    return {{"conflict_id", value.conflict_id}, {"criterion_id", value.criterion_id},
            {"evidence_ids", value.evidence_ids}, {"conflict_kind", value.conflict_kind},
            {"resolved", value.resolved}, {"resolution", value.resolution}};
}

EvidenceConflict conflict_value(const json& value) {
    static const std::set<std::string> fields = {
        "conflict_id", "criterion_id", "evidence_ids", "conflict_kind", "resolved", "resolution"};
    if(!contracts::validate_object_fields(value, fields, fields,
        contracts::UnknownFieldPolicy::Reject, nullptr, nullptr, "/conflict"))
        throw std::invalid_argument("invalid evidence conflict fields");
    auto result = EvidenceConflict{value.at("conflict_id").get<std::string>(),
            value.at("criterion_id").get<std::string>(),
            value.at("evidence_ids").get<std::vector<std::string>>(),
            value.at("conflict_kind").get<std::string>(),
            value.at("resolved").get<bool>(), value.at("resolution").get<std::string>()};
    if(result.conflict_id.empty() || result.criterion_id.empty() || result.evidence_ids.empty())
        throw std::invalid_argument("evidence conflict identity and evidence are required");
    return result;
}

json resolution_payload(const ResolutionReport& value) {
    json findings = json::array();
    for(const auto& item : value.normalized_findings) findings.push_back(finding_json(item));
    json conflicts = json::array();
    for(const auto& item : value.conflicts) conflicts.push_back(conflict_json(item));
    return {{"workflow_id", value.workflow_id}, {"normalized_findings", std::move(findings)},
            {"conflicts", std::move(conflicts)}, {"residual_risks", value.residual_risks},
            {"resolver_invocation_id", value.resolver_invocation_id}};
}

ResolutionReport resolution_from_payload(const json& payload) {
    ResolutionReport result;
    result.workflow_id = payload.at("workflow_id").get<std::string>();
    for(const auto& item : payload.at("normalized_findings"))
        result.normalized_findings.push_back(finding_value(item));
    for(const auto& item : payload.at("conflicts")) result.conflicts.push_back(conflict_value(item));
    result.residual_risks = payload.at("residual_risks").get<std::vector<std::string>>();
    result.resolver_invocation_id = payload.at("resolver_invocation_id").get<std::string>();
    if(result.workflow_id.empty()) throw std::invalid_argument("resolution workflow id is required");
    return result;
}

json artifact_json(const AssuranceStageArtifact& value) {
    return {{"stage", assurance_stage_name(value.stage)}, {"attempt", value.attempt},
            {"invocation_id", value.invocation_id}, {"manifest_digest", value.manifest_digest},
            {"output_digest", value.output_digest}, {"provider", value.provider},
            {"model", value.model}, {"independence_group", value.independence_group},
            {"calibration_revision", value.calibration_revision},
            {"output", value.output}};
}

AssuranceStageArtifact artifact_value(const json& value) {
    static const std::set<std::string> fields = {
        "stage", "attempt", "invocation_id", "manifest_digest", "output_digest", "provider",
        "model", "independence_group", "calibration_revision", "output"};
    if(!contracts::validate_object_fields(value, fields, fields,
        contracts::UnknownFieldPolicy::Reject, nullptr, nullptr, "/artifact"))
        throw std::invalid_argument("invalid assurance stage artifact fields");
    const auto stage = assurance_stage_from_name(value.at("stage").get<std::string>());
    if(!stage) throw std::invalid_argument("invalid assurance stage artifact");
    auto result = AssuranceStageArtifact{*stage, value.at("attempt").get<std::uint64_t>(),
            value.at("invocation_id").get<std::string>(),
            value.at("manifest_digest").get<std::string>(),
            value.at("output_digest").get<std::string>(),
            value.at("provider").get<std::string>(), value.at("model").get<std::string>(),
            value.at("independence_group").get<std::string>(),
            value.at("calibration_revision").get<std::string>(), value.at("output")};
    if(result.attempt == 0 || result.invocation_id.empty() || result.manifest_digest.empty() ||
       result.output_digest.empty() || result.provider.empty() || result.model.empty() ||
       result.independence_group.empty())
        throw std::invalid_argument("assurance stage artifact manifest bindings are required");
    return result;
}

template <typename T, typename Builder>
std::optional<T> decode_typed(const json& value, const char* kind,
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

std::string professional_role_name(ProfessionalRole value) {
    switch(value) {
        case ProfessionalRole::VerificationPlanner: return "verification_planner";
        case ProfessionalRole::Code: return "code";
        case ProfessionalRole::Architecture: return "architecture";
        case ProfessionalRole::Domain: return "domain";
        case ProfessionalRole::Security: return "security";
        case ProfessionalRole::Completeness: return "completeness";
        case ProfessionalRole::EvidenceResolver: return "evidence_resolver";
    }
    return "code";
}

std::optional<ProfessionalRole> professional_role_from_name(std::string_view value) {
    if(value == "verification_planner") return ProfessionalRole::VerificationPlanner;
    if(value == "code") return ProfessionalRole::Code;
    if(value == "architecture") return ProfessionalRole::Architecture;
    if(value == "domain") return ProfessionalRole::Domain;
    if(value == "security") return ProfessionalRole::Security;
    if(value == "completeness") return ProfessionalRole::Completeness;
    if(value == "evidence_resolver") return ProfessionalRole::EvidenceResolver;
    return std::nullopt;
}

std::string assurance_stage_name(AssuranceStage value) {
    switch(value) {
        case AssuranceStage::Planning: return "planning";
        case AssuranceStage::DeterministicEvidence: return "deterministic_evidence";
        case AssuranceStage::CodeVerification: return "code_verification";
        case AssuranceStage::ArchitectureVerification: return "architecture_verification";
        case AssuranceStage::DomainVerification: return "domain_verification";
        case AssuranceStage::SecurityVerification: return "security_verification";
        case AssuranceStage::CompletenessVerification: return "completeness_verification";
        case AssuranceStage::EvidenceResolution: return "evidence_resolution";
        case AssuranceStage::Arbitration: return "arbitration";
        case AssuranceStage::Complete: return "complete";
    }
    return "planning";
}

std::optional<AssuranceStage> assurance_stage_from_name(std::string_view value) {
    if(value == "planning") return AssuranceStage::Planning;
    if(value == "deterministic_evidence") return AssuranceStage::DeterministicEvidence;
    if(value == "code_verification") return AssuranceStage::CodeVerification;
    if(value == "architecture_verification") return AssuranceStage::ArchitectureVerification;
    if(value == "domain_verification") return AssuranceStage::DomainVerification;
    if(value == "security_verification") return AssuranceStage::SecurityVerification;
    if(value == "completeness_verification") return AssuranceStage::CompletenessVerification;
    if(value == "evidence_resolution") return AssuranceStage::EvidenceResolution;
    if(value == "arbitration") return AssuranceStage::Arbitration;
    if(value == "complete") return AssuranceStage::Complete;
    return std::nullopt;
}

std::string assurance_workflow_state_name(AssuranceWorkflowState value) {
    switch(value) {
        case AssuranceWorkflowState::Running: return "running";
        case AssuranceWorkflowState::Completed: return "completed";
        case AssuranceWorkflowState::Failed: return "failed";
        case AssuranceWorkflowState::Cancelled: return "cancelled";
        case AssuranceWorkflowState::ManualReview: return "manual_review";
    }
    return "failed";
}

std::optional<AssuranceWorkflowState> assurance_workflow_state_from_name(std::string_view value) {
    if(value == "running") return AssuranceWorkflowState::Running;
    if(value == "completed") return AssuranceWorkflowState::Completed;
    if(value == "failed") return AssuranceWorkflowState::Failed;
    if(value == "cancelled") return AssuranceWorkflowState::Cancelled;
    if(value == "manual_review") return AssuranceWorkflowState::ManualReview;
    return std::nullopt;
}

json encode(const VerificationPlan& value) {
    return contracts::make_typed_contract(value.metadata, "agent.verification_plan/v1",
                                          plan_payload(value));
}

json encode(const ResolutionReport& value) {
    return contracts::make_typed_contract(value.metadata, "agent.evidence_resolution/v1",
                                          resolution_payload(value));
}

json encode(const AssuranceCheckpoint& value) {
    json evidence = json::array();
    for(const auto& item : value.evidence) evidence.push_back(evidence_json(item));
    json findings = json::array();
    for(const auto& item : value.findings) findings.push_back(finding_json(item));
    json artifacts = json::array();
    for(const auto& item : value.artifacts) artifacts.push_back(artifact_json(item));
    return contracts::make_typed_contract(value.metadata, "agent.assurance_checkpoint/v1",
        {{"workflow_id", value.workflow_id}, {"revision", value.revision},
         {"state", assurance_workflow_state_name(value.state)},
         {"next_stage", assurance_stage_name(value.next_stage)},
         {"stage_attempts", value.stage_attempts}, {"completed_stages", value.completed_stages},
         {"acceptance_contract_digest", value.acceptance_contract_digest},
         {"task_context_digest", value.task_context_digest},
         {"artifact_manifest_digest", value.artifact_manifest_digest},
         {"memory_snapshot_id", value.memory_snapshot_id},
         {"memory_view_digest", value.memory_view_digest},
         {"verification_plan", value.verification_plan ? encode(*value.verification_plan) : json(nullptr)},
         {"evidence", std::move(evidence)}, {"findings", std::move(findings)},
         {"artifacts", std::move(artifacts)},
         {"resolution", value.resolution ? encode(*value.resolution) : json(nullptr)},
         {"acceptance_report_digest", value.acceptance_report_digest},
         {"error_code", value.error_code}, {"error_message", value.error_message},
         {"updated_at", value.updated_at}});
}

std::optional<VerificationPlan> decode_verification_plan(
    const json& value, const contracts::ParseContext& context,
    std::vector<ContractIssue>* issues) {
    static const std::set<std::string> fields = {
        "verification_plan_id", "revision", "acceptance_contract_digest", "task_plan_digest",
        "artifact_manifest_digest", "assignments", "planner_invocation_id", "created_at"};
    return decode_typed<VerificationPlan>(value, "agent.verification_plan/v1", fields,
        context, issues, [](const json& payload) { return plan_from_payload(payload); });
}

std::optional<ResolutionReport> decode_resolution_report(
    const json& value, const contracts::ParseContext& context,
    std::vector<ContractIssue>* issues) {
    static const std::set<std::string> fields = {
        "workflow_id", "normalized_findings", "conflicts", "residual_risks",
        "resolver_invocation_id"};
    return decode_typed<ResolutionReport>(value, "agent.evidence_resolution/v1", fields,
        context, issues, [](const json& payload) { return resolution_from_payload(payload); });
}

std::optional<AssuranceCheckpoint> decode_assurance_checkpoint(
    const json& value, const contracts::ParseContext& context,
    std::vector<ContractIssue>* issues) {
    static const std::set<std::string> fields = {
        "workflow_id", "revision", "state", "next_stage", "stage_attempts",
        "completed_stages", "acceptance_contract_digest", "task_context_digest",
        "artifact_manifest_digest", "memory_snapshot_id", "memory_view_digest",
        "verification_plan", "evidence", "findings", "artifacts", "resolution",
        "acceptance_report_digest", "error_code", "error_message", "updated_at"};
    return decode_typed<AssuranceCheckpoint>(value, "agent.assurance_checkpoint/v1", fields,
        context, issues, [context](const json& payload) {
            AssuranceCheckpoint result;
            result.workflow_id = payload.at("workflow_id").get<std::string>();
            result.revision = payload.at("revision").get<std::uint64_t>();
            const auto state = assurance_workflow_state_from_name(payload.at("state").get<std::string>());
            const auto stage = assurance_stage_from_name(payload.at("next_stage").get<std::string>());
            if(!state || !stage) throw std::invalid_argument("invalid assurance checkpoint state");
            result.state = *state;
            result.next_stage = *stage;
            result.stage_attempts = payload.at("stage_attempts").get<std::map<std::string, std::uint64_t>>();
            result.completed_stages = payload.at("completed_stages").get<std::vector<std::string>>();
            result.acceptance_contract_digest = payload.at("acceptance_contract_digest").get<std::string>();
            result.task_context_digest = payload.at("task_context_digest").get<std::string>();
            result.artifact_manifest_digest = payload.at("artifact_manifest_digest").get<std::string>();
            result.memory_snapshot_id = payload.at("memory_snapshot_id").get<std::string>();
            result.memory_view_digest = payload.at("memory_view_digest").get<std::string>();
            if(!payload.at("verification_plan").is_null()) {
                auto plan = decode_verification_plan(payload.at("verification_plan"), context);
                if(!plan) throw std::invalid_argument("invalid embedded verification plan");
                result.verification_plan = std::move(*plan);
            }
            for(const auto& item : payload.at("evidence")) result.evidence.push_back(evidence_value(item));
            for(const auto& item : payload.at("findings")) result.findings.push_back(finding_value(item));
            for(const auto& item : payload.at("artifacts")) result.artifacts.push_back(artifact_value(item));
            if(!payload.at("resolution").is_null()) {
                auto report = decode_resolution_report(payload.at("resolution"), context);
                if(!report) throw std::invalid_argument("invalid embedded resolution report");
                result.resolution = std::move(*report);
            }
            result.acceptance_report_digest = payload.at("acceptance_report_digest").get<std::string>();
            result.error_code = payload.at("error_code").get<std::string>();
            result.error_message = payload.at("error_message").get<std::string>();
            result.updated_at = payload.at("updated_at").get<std::string>();
            if(result.workflow_id.empty() || result.revision == 0 ||
               result.acceptance_contract_digest.empty() || result.task_context_digest.empty() ||
               result.artifact_manifest_digest.empty())
                throw std::invalid_argument("assurance checkpoint bindings are required");
            return result;
        });
}

}  // namespace agent_framework::assurance
