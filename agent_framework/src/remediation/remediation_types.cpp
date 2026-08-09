#include "agent/remediation/remediation_workflow.hpp"

#include <set>
#include <stdexcept>

namespace agent_framework::remediation {
namespace {
using json = nlohmann::json;

void require_fields(const json& value, const std::set<std::string>& fields,
                    std::string_view path) {
    if(!contracts::validate_object_fields(value, fields, fields,
        contracts::UnknownFieldPolicy::Reject, nullptr, nullptr, path))
        throw std::invalid_argument("invalid or unknown fields at " + std::string(path));
}

json requirement_json(const RequirementBinding& v) {
    return {{"requirement_id", v.requirement_id}, {"criterion_ids", v.criterion_ids},
            {"plan_node_ids", v.plan_node_ids}, {"artifact_ids", v.artifact_ids}};
}
RequirementBinding requirement_value(const json& v) {
    require_fields(v, {"requirement_id", "criterion_ids", "plan_node_ids", "artifact_ids"},
                   "/requirement");
    return {v.at("requirement_id").get<std::string>(),
            v.at("criterion_ids").get<std::vector<std::string>>(),
            v.at("plan_node_ids").get<std::vector<std::string>>(),
            v.at("artifact_ids").get<std::vector<std::string>>()};
}
json artifact_json(const ArtifactBinding& v) {
    return {{"artifact_id", v.artifact_id}, {"content_digest", v.content_digest},
            {"producer_node_id", v.producer_node_id}, {"criterion_ids", v.criterion_ids},
            {"depends_on_artifact_ids", v.depends_on_artifact_ids},
            {"evidence_ids", v.evidence_ids}, {"memory_record_ids", v.memory_record_ids},
            {"verifier_roles", v.verifier_roles}};
}
ArtifactBinding artifact_value(const json& v) {
    require_fields(v, {"artifact_id", "content_digest", "producer_node_id", "criterion_ids",
        "depends_on_artifact_ids", "evidence_ids", "memory_record_ids", "verifier_roles"},
        "/artifact");
    return {v.at("artifact_id").get<std::string>(), v.at("content_digest").get<std::string>(),
            v.at("producer_node_id").get<std::string>(),
            v.at("criterion_ids").get<std::vector<std::string>>(),
            v.at("depends_on_artifact_ids").get<std::vector<std::string>>(),
            v.at("evidence_ids").get<std::vector<std::string>>(),
            v.at("memory_record_ids").get<std::vector<std::string>>(),
            v.at("verifier_roles").get<std::vector<std::string>>()};
}
json evidence_json(const EvidenceBinding& v) {
    return {{"evidence_id", v.evidence_id}, {"criterion_id", v.criterion_id},
            {"artifact_id", v.artifact_id}, {"source_kind", v.source_kind},
            {"content_digest", v.content_digest}, {"freshness_deadline", v.freshness_deadline},
            {"strong_oracle", v.strong_oracle}};
}
EvidenceBinding evidence_value(const json& v) {
    require_fields(v, {"evidence_id", "criterion_id", "artifact_id", "source_kind",
        "content_digest", "freshness_deadline", "strong_oracle"}, "/evidence");
    return {v.at("evidence_id").get<std::string>(), v.at("criterion_id").get<std::string>(),
            v.at("artifact_id").get<std::string>(), v.at("source_kind").get<std::string>(),
            v.at("content_digest").get<std::string>(),
            v.at("freshness_deadline").get<std::string>(), v.at("strong_oracle").get<bool>()};
}
json finding_binding_json(const FindingBinding& v) {
    return {{"finding_id", v.finding_id}, {"criterion_id", v.criterion_id},
            {"requirement_ids", v.requirement_ids}, {"plan_node_ids", v.plan_node_ids},
            {"artifact_ids", v.artifact_ids}, {"evidence_ids", v.evidence_ids}};
}
FindingBinding finding_binding_value(const json& v) {
    require_fields(v, {"finding_id", "criterion_id", "requirement_ids", "plan_node_ids",
        "artifact_ids", "evidence_ids"}, "/finding_binding");
    return {v.at("finding_id").get<std::string>(), v.at("criterion_id").get<std::string>(),
            v.at("requirement_ids").get<std::vector<std::string>>(),
            v.at("plan_node_ids").get<std::vector<std::string>>(),
            v.at("artifact_ids").get<std::vector<std::string>>(),
            v.at("evidence_ids").get<std::vector<std::string>>()};
}
json impact_payload(const ImpactGraph& v) {
    json bindings = json::array();
    for(const auto& item : v.finding_bindings) bindings.push_back(finding_binding_json(item));
    return {{"graph_id", v.graph_id}, {"inventory_digest", v.inventory_digest},
            {"acceptance_report_digest", v.acceptance_report_digest},
            {"finding_bindings", std::move(bindings)},
            {"affected_criterion_ids", v.affected_criterion_ids},
            {"affected_plan_node_ids", v.affected_plan_node_ids},
            {"invalidated_artifact_ids", v.invalidated_artifact_ids},
            {"invalidated_evidence_ids", v.invalidated_evidence_ids},
            {"invalidated_memory_record_ids", v.invalidated_memory_record_ids},
            {"verifier_roles", v.verifier_roles}};
}
ImpactGraph impact_value(const json& p) {
    require_fields(p, {"graph_id", "inventory_digest", "acceptance_report_digest",
        "finding_bindings", "affected_criterion_ids", "affected_plan_node_ids",
        "invalidated_artifact_ids", "invalidated_evidence_ids",
        "invalidated_memory_record_ids", "verifier_roles"}, "/impact_graph");
    ImpactGraph v;
    v.graph_id = p.at("graph_id").get<std::string>();
    v.inventory_digest = p.at("inventory_digest").get<std::string>();
    v.acceptance_report_digest = p.at("acceptance_report_digest").get<std::string>();
    for(const auto& item : p.at("finding_bindings")) v.finding_bindings.push_back(finding_binding_value(item));
    v.affected_criterion_ids = p.at("affected_criterion_ids").get<std::vector<std::string>>();
    v.affected_plan_node_ids = p.at("affected_plan_node_ids").get<std::vector<std::string>>();
    v.invalidated_artifact_ids = p.at("invalidated_artifact_ids").get<std::vector<std::string>>();
    v.invalidated_evidence_ids = p.at("invalidated_evidence_ids").get<std::vector<std::string>>();
    v.invalidated_memory_record_ids = p.at("invalidated_memory_record_ids").get<std::vector<std::string>>();
    v.verifier_roles = p.at("verifier_roles").get<std::vector<std::string>>();
    return v;
}
json change_json(const CriterionChange& v) {
    return {{"criterion_id", v.criterion_id}, {"old_mandatory", v.old_mandatory},
            {"new_mandatory", v.new_mandatory}, {"old_threshold", v.old_threshold},
            {"new_threshold", v.new_threshold}, {"rationale", v.rationale}};
}
CriterionChange change_value(const json& v) {
    require_fields(v, {"criterion_id", "old_mandatory", "new_mandatory", "old_threshold",
        "new_threshold", "rationale"}, "/criterion_change");
    return {v.at("criterion_id").get<std::string>(), v.at("old_mandatory").get<bool>(),
            v.at("new_mandatory").get<bool>(), v.at("old_threshold").get<std::string>(),
            v.at("new_threshold").get<std::string>(), v.at("rationale").get<std::string>()};
}
json action_json(const RemediationAction& v) {
    return {{"action_id", v.action_id}, {"objective", v.objective},
            {"finding_ids", v.finding_ids}, {"affected_plan_node_ids", v.affected_plan_node_ids},
            {"affected_artifact_ids", v.affected_artifact_ids},
            {"required_capabilities", v.required_capabilities}, {"side_effects", v.side_effects},
            {"output_contracts", v.output_contracts}, {"rollback_strategy", v.rollback_strategy},
            {"risk_level", v.risk_level}, {"approval_required", v.approval_required}};
}
RemediationAction action_value(const json& v) {
    require_fields(v, {"action_id", "objective", "finding_ids", "affected_plan_node_ids",
        "affected_artifact_ids", "required_capabilities", "side_effects", "output_contracts",
        "rollback_strategy", "risk_level", "approval_required"}, "/remediation_action");
    return {v.at("action_id").get<std::string>(), v.at("objective").get<std::string>(),
            v.at("finding_ids").get<std::vector<std::string>>(),
            v.at("affected_plan_node_ids").get<std::vector<std::string>>(),
            v.at("affected_artifact_ids").get<std::vector<std::string>>(),
            v.at("required_capabilities").get<std::vector<std::string>>(),
            v.at("side_effects").get<std::vector<std::string>>(),
            v.at("output_contracts").get<std::vector<std::string>>(),
            v.at("rollback_strategy").get<std::string>(), v.at("risk_level").get<std::string>(),
            v.at("approval_required").get<bool>()};
}
json remediation_payload(const RemediationPlan& v) {
    json actions = json::array(), changes = json::array();
    for(const auto& item : v.actions) actions.push_back(action_json(item));
    for(const auto& item : v.criterion_changes) changes.push_back(change_json(item));
    return {{"remediation_id", v.remediation_id}, {"revision", v.revision},
            {"parent_plan_digest", v.parent_plan_digest},
            {"acceptance_report_digest", v.acceptance_report_digest},
            {"impact_graph_digest", v.impact_graph_digest}, {"actions", std::move(actions)},
            {"criterion_changes", std::move(changes)}, {"planner_invocation_id", v.planner_invocation_id}};
}
RemediationPlan remediation_value(const json& p) {
    require_fields(p, {"remediation_id", "revision", "parent_plan_digest",
        "acceptance_report_digest", "impact_graph_digest", "actions", "criterion_changes",
        "planner_invocation_id"}, "/remediation_plan");
    RemediationPlan v;
    v.remediation_id = p.at("remediation_id").get<std::string>();
    v.revision = p.at("revision").get<std::uint64_t>();
    v.parent_plan_digest = p.at("parent_plan_digest").get<std::string>();
    v.acceptance_report_digest = p.at("acceptance_report_digest").get<std::string>();
    v.impact_graph_digest = p.at("impact_graph_digest").get<std::string>();
    for(const auto& item : p.at("actions")) v.actions.push_back(action_value(item));
    for(const auto& item : p.at("criterion_changes")) v.criterion_changes.push_back(change_value(item));
    v.planner_invocation_id = p.at("planner_invocation_id").get<std::string>();
    return v;
}
json reverify_payload(const ReverificationPlan& v) {
    return {{"reverification_id", v.reverification_id},
            {"remediation_plan_digest", v.remediation_plan_digest},
            {"proposed_plan_digest", v.proposed_plan_digest}, {"criterion_ids", v.criterion_ids},
            {"forced_oracle_kinds", v.forced_oracle_kinds}, {"verifier_roles", v.verifier_roles},
            {"invalidated_evidence_ids", v.invalidated_evidence_ids},
            {"reusable_evidence_ids", v.reusable_evidence_ids},
            {"baseline_artifact_digests", v.baseline_artifact_digests}, {"created_at", v.created_at}};
}
ReverificationPlan reverify_value(const json& p) {
    require_fields(p, {"reverification_id", "remediation_plan_digest", "proposed_plan_digest",
        "criterion_ids", "forced_oracle_kinds", "verifier_roles", "invalidated_evidence_ids",
        "reusable_evidence_ids", "baseline_artifact_digests", "created_at"},
        "/reverification_plan");
    ReverificationPlan v;
    v.reverification_id = p.at("reverification_id").get<std::string>();
    v.remediation_plan_digest = p.at("remediation_plan_digest").get<std::string>();
    v.proposed_plan_digest = p.at("proposed_plan_digest").get<std::string>();
    v.criterion_ids = p.at("criterion_ids").get<std::vector<std::string>>();
    v.forced_oracle_kinds = p.at("forced_oracle_kinds").get<std::vector<std::string>>();
    v.verifier_roles = p.at("verifier_roles").get<std::vector<std::string>>();
    v.invalidated_evidence_ids = p.at("invalidated_evidence_ids").get<std::vector<std::string>>();
    v.reusable_evidence_ids = p.at("reusable_evidence_ids").get<std::vector<std::string>>();
    v.baseline_artifact_digests = p.at("baseline_artifact_digests").get<std::map<std::string, std::string>>();
    v.created_at = p.at("created_at").get<std::string>();
    return v;
}
json stage_artifact_json(const RemediationStageArtifact& v) {
    return {{"stage", remediation_stage_name(v.stage)}, {"attempt", v.attempt},
            {"invocation_id", v.invocation_id}, {"output_digest", v.output_digest},
            {"provider", v.provider}, {"model", v.model}, {"tokens", v.tokens},
            {"cost_usd", v.cost_usd}, {"output", v.output}};
}
RemediationStageArtifact stage_artifact_value(const json& p) {
    require_fields(p, {"stage", "attempt", "invocation_id", "output_digest", "provider",
        "model", "tokens", "cost_usd", "output"}, "/stage_artifact");
    const auto stage = remediation_stage_from_name(p.at("stage").get<std::string>());
    if(!stage) throw std::invalid_argument("unknown remediation stage");
    return {*stage, p.at("attempt").get<std::uint64_t>(), p.at("invocation_id").get<std::string>(),
            p.at("output_digest").get<std::string>(), p.at("provider").get<std::string>(),
            p.at("model").get<std::string>(), p.at("tokens").get<std::uint64_t>(),
            p.at("cost_usd").get<double>(), p.at("output")};
}

template <class T, class Builder>
std::optional<T> decode_document(const json& value, const char* kind,
                                 const std::set<std::string>& fields,
                                 const contracts::ParseContext& context,
                                 std::vector<contracts::ContractIssue>* issues, Builder builder) {
    auto document = contracts::parse_typed_contract(value, kind, context, issues);
    if(!document || !contracts::validate_object_fields(document->payload, fields, fields,
        contracts::UnknownFieldPolicy::Reject, nullptr, issues, "/payload")) return std::nullopt;
    try {
        T result = builder(document->payload);
        result.metadata = std::move(document->metadata);
        return result;
    } catch(const std::exception& e) {
        contracts::append_issue(issues, "payload_decode_failed", "/payload", e.what());
        return std::nullopt;
    }
}
}  // namespace

std::string remediation_stage_name(RemediationStage v) {
    switch(v) {
        case RemediationStage::ImpactAnalysis: return "impact_analysis";
        case RemediationStage::RemediationPlanning: return "remediation_planning";
        case RemediationStage::PolicyGate: return "policy_gate";
        case RemediationStage::PlanCommit: return "plan_commit";
        case RemediationStage::ReverificationPlanning: return "reverification_planning";
        case RemediationStage::Complete: return "complete";
    }
    return "impact_analysis";
}
std::optional<RemediationStage> remediation_stage_from_name(std::string_view v) {
    if(v == "impact_analysis") return RemediationStage::ImpactAnalysis;
    if(v == "remediation_planning") return RemediationStage::RemediationPlanning;
    if(v == "policy_gate") return RemediationStage::PolicyGate;
    if(v == "plan_commit") return RemediationStage::PlanCommit;
    if(v == "reverification_planning") return RemediationStage::ReverificationPlanning;
    if(v == "complete") return RemediationStage::Complete;
    return std::nullopt;
}
std::string remediation_state_name(RemediationState v) {
    switch(v) {
        case RemediationState::Running: return "running";
        case RemediationState::AwaitingApproval: return "awaiting_approval";
        case RemediationState::ReadyForExecution: return "ready_for_execution";
        case RemediationState::ManualReview: return "manual_review";
        case RemediationState::Failed: return "failed";
        case RemediationState::Cancelled: return "cancelled";
    }
    return "failed";
}
std::optional<RemediationState> remediation_state_from_name(std::string_view v) {
    if(v == "running") return RemediationState::Running;
    if(v == "awaiting_approval") return RemediationState::AwaitingApproval;
    if(v == "ready_for_execution") return RemediationState::ReadyForExecution;
    if(v == "manual_review") return RemediationState::ManualReview;
    if(v == "failed") return RemediationState::Failed;
    if(v == "cancelled") return RemediationState::Cancelled;
    return std::nullopt;
}

json encode(const ImpactInventory& v) {
    json requirements = json::array(), artifacts = json::array(), evidence = json::array();
    for(const auto& item : v.requirements) requirements.push_back(requirement_json(item));
    for(const auto& item : v.artifacts) artifacts.push_back(artifact_json(item));
    for(const auto& item : v.evidence) evidence.push_back(evidence_json(item));
    return contracts::make_typed_contract(v.metadata, "agent.remediation_impact_inventory/v1",
        {{"inventory_id", v.inventory_id}, {"plan_digest", v.plan_digest},
         {"artifact_manifest_digest", v.artifact_manifest_digest},
         {"requirements", std::move(requirements)}, {"artifacts", std::move(artifacts)},
         {"evidence", std::move(evidence)}});
}
json encode(const ImpactGraph& v) {
    return contracts::make_typed_contract(v.metadata, "agent.remediation_impact_graph/v1", impact_payload(v));
}
json encode(const RemediationPlan& v) {
    return contracts::make_typed_contract(v.metadata, "agent.remediation_plan/v1", remediation_payload(v));
}
json encode(const ReverificationPlan& v) {
    return contracts::make_typed_contract(v.metadata, "agent.reverification_plan/v1", reverify_payload(v));
}
json encode(const RemediationCheckpoint& v) {
    json artifacts = json::array();
    for(const auto& item : v.artifacts) artifacts.push_back(stage_artifact_json(item));
    json payload = {{"workflow_id", v.workflow_id}, {"revision", v.revision},
        {"state", remediation_state_name(v.state)}, {"next_stage", remediation_stage_name(v.next_stage)},
        {"stage_attempts", v.stage_attempts}, {"completed_stages", v.completed_stages},
        {"current_plan_digest", v.current_plan_digest},
        {"acceptance_contract_digest", v.acceptance_contract_digest},
        {"acceptance_report_digest", v.acceptance_report_digest},
        {"assurance_checkpoint_digest", v.assurance_checkpoint_digest},
        {"impact_inventory_digest", v.impact_inventory_digest},
        {"memory_snapshot_id", v.memory_snapshot_id}, {"memory_view_digest", v.memory_view_digest},
        {"impact_graph", v.impact_graph ? impact_payload(*v.impact_graph) : json(nullptr)},
        {"remediation_plan", v.remediation_plan ? remediation_payload(*v.remediation_plan) : json(nullptr)},
        {"proposed_plan", v.proposed_plan ? planning::encode(*v.proposed_plan) : json(nullptr)},
        {"reverification_plan", v.reverification_plan ? reverify_payload(*v.reverification_plan) : json(nullptr)},
        {"artifacts", std::move(artifacts)}, {"proposal_signatures", v.proposal_signatures},
        {"consumed_tokens", v.consumed_tokens}, {"consumed_cost_usd", v.consumed_cost_usd},
        {"approval_request_digest", v.approval_request_digest},
        {"approval_decision_id", v.approval_decision_id},
        {"committed_plan_digest", v.committed_plan_digest}, {"error_code", v.error_code},
        {"error_message", v.error_message}, {"updated_at", v.updated_at}};
    return contracts::make_typed_contract(v.metadata, "agent.remediation_checkpoint/v1", std::move(payload));
}

std::optional<ImpactInventory> decode_impact_inventory(const json& value,
    const contracts::ParseContext& context, std::vector<contracts::ContractIssue>* issues) {
    static const std::set<std::string> fields = {"inventory_id", "plan_digest",
        "artifact_manifest_digest", "requirements", "artifacts", "evidence"};
    return decode_document<ImpactInventory>(value, "agent.remediation_impact_inventory/v1", fields,
        context, issues, [](const json& p) {
            ImpactInventory v;
            v.inventory_id = p.at("inventory_id").get<std::string>();
            v.plan_digest = p.at("plan_digest").get<std::string>();
            v.artifact_manifest_digest = p.at("artifact_manifest_digest").get<std::string>();
            for(const auto& item : p.at("requirements")) v.requirements.push_back(requirement_value(item));
            for(const auto& item : p.at("artifacts")) v.artifacts.push_back(artifact_value(item));
            for(const auto& item : p.at("evidence")) v.evidence.push_back(evidence_value(item));
            return v;
        });
}

std::optional<ImpactGraph> decode_impact_graph(const json& value,
    const contracts::ParseContext& context, std::vector<contracts::ContractIssue>* issues) {
    static const std::set<std::string> fields = {"graph_id", "inventory_digest",
        "acceptance_report_digest", "finding_bindings", "affected_criterion_ids",
        "affected_plan_node_ids", "invalidated_artifact_ids", "invalidated_evidence_ids",
        "invalidated_memory_record_ids", "verifier_roles"};
    return decode_document<ImpactGraph>(value, "agent.remediation_impact_graph/v1", fields,
        context, issues, [](const json& p) { return impact_value(p); });
}

std::optional<RemediationPlan> decode_remediation_plan(const json& value,
    const contracts::ParseContext& context, std::vector<contracts::ContractIssue>* issues) {
    static const std::set<std::string> fields = {"remediation_id", "revision",
        "parent_plan_digest", "acceptance_report_digest", "impact_graph_digest", "actions",
        "criterion_changes", "planner_invocation_id"};
    return decode_document<RemediationPlan>(value, "agent.remediation_plan/v1", fields,
        context, issues, [](const json& p) { return remediation_value(p); });
}

std::optional<ReverificationPlan> decode_reverification_plan(const json& value,
    const contracts::ParseContext& context, std::vector<contracts::ContractIssue>* issues) {
    static const std::set<std::string> fields = {"reverification_id", "remediation_plan_digest",
        "proposed_plan_digest", "criterion_ids", "forced_oracle_kinds", "verifier_roles",
        "invalidated_evidence_ids", "reusable_evidence_ids", "baseline_artifact_digests",
        "created_at"};
    return decode_document<ReverificationPlan>(value, "agent.reverification_plan/v1", fields,
        context, issues, [](const json& p) { return reverify_value(p); });
}

std::optional<RemediationCheckpoint> decode_remediation_checkpoint(const json& value,
    const contracts::ParseContext& context, std::vector<contracts::ContractIssue>* issues) {
    static const std::set<std::string> fields = {"workflow_id", "revision", "state", "next_stage",
        "stage_attempts", "completed_stages", "current_plan_digest", "acceptance_contract_digest",
        "acceptance_report_digest", "assurance_checkpoint_digest", "impact_inventory_digest",
        "memory_snapshot_id", "memory_view_digest", "impact_graph", "remediation_plan",
        "proposed_plan", "reverification_plan", "artifacts", "proposal_signatures",
        "consumed_tokens", "consumed_cost_usd", "approval_request_digest", "approval_decision_id",
        "committed_plan_digest", "error_code", "error_message", "updated_at"};
    return decode_document<RemediationCheckpoint>(value, "agent.remediation_checkpoint/v1", fields,
        context, issues, [](const json& p) {
            RemediationCheckpoint v;
            v.workflow_id = p.at("workflow_id").get<std::string>();
            v.revision = p.at("revision").get<std::uint64_t>();
            auto state = remediation_state_from_name(p.at("state").get<std::string>());
            auto stage = remediation_stage_from_name(p.at("next_stage").get<std::string>());
            if(!state || !stage) throw std::invalid_argument("unknown remediation state/stage");
            v.state = *state; v.next_stage = *stage;
            v.stage_attempts = p.at("stage_attempts").get<std::map<std::string, std::uint64_t>>();
            v.completed_stages = p.at("completed_stages").get<std::vector<std::string>>();
            v.current_plan_digest = p.at("current_plan_digest").get<std::string>();
            v.acceptance_contract_digest = p.at("acceptance_contract_digest").get<std::string>();
            v.acceptance_report_digest = p.at("acceptance_report_digest").get<std::string>();
            v.assurance_checkpoint_digest = p.at("assurance_checkpoint_digest").get<std::string>();
            v.impact_inventory_digest = p.at("impact_inventory_digest").get<std::string>();
            v.memory_snapshot_id = p.at("memory_snapshot_id").get<std::string>();
            v.memory_view_digest = p.at("memory_view_digest").get<std::string>();
            if(!p.at("impact_graph").is_null()) v.impact_graph = impact_value(p.at("impact_graph"));
            if(!p.at("remediation_plan").is_null()) v.remediation_plan = remediation_value(p.at("remediation_plan"));
            if(!p.at("proposed_plan").is_null()) {
                auto plan = planning::decode_execution_plan(p.at("proposed_plan"));
                if(!plan) throw std::invalid_argument("invalid proposed plan");
                v.proposed_plan = std::move(*plan);
            }
            if(!p.at("reverification_plan").is_null()) v.reverification_plan = reverify_value(p.at("reverification_plan"));
            for(const auto& item : p.at("artifacts")) v.artifacts.push_back(stage_artifact_value(item));
            v.proposal_signatures = p.at("proposal_signatures").get<std::vector<std::string>>();
            v.consumed_tokens = p.at("consumed_tokens").get<std::uint64_t>();
            v.consumed_cost_usd = p.at("consumed_cost_usd").get<double>();
            v.approval_request_digest = p.at("approval_request_digest").get<std::string>();
            v.approval_decision_id = p.at("approval_decision_id").get<std::string>();
            v.committed_plan_digest = p.at("committed_plan_digest").get<std::string>();
            v.error_code = p.at("error_code").get<std::string>();
            v.error_message = p.at("error_message").get<std::string>();
            v.updated_at = p.at("updated_at").get<std::string>();
            return v;
        });
}

}  // namespace agent_framework::remediation
