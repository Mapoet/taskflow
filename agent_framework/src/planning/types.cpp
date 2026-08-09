#include "agent/planning/types.hpp"

#include <set>
#include <utility>

namespace agent_framework::planning {
namespace {

using json = nlohmann::json;
using contracts::ContractIssue;

const std::set<std::string> kIntakeFields = {
    "user_goal", "requested_deliverables", "explicit_constraints",
    "granted_authorities", "success_signals", "fact_gaps"};
const std::set<std::string> kEvidenceBundleFields = {"bundle_id", "records"};
const std::set<std::string> kUnderstandingFields = {
    "domain", "current_state", "target_state", "gaps", "assumptions", "unknowns",
    "risks", "evidence_ids", "change_mode", "blast_radius"};
const std::set<std::string> kPlanFields = {
    "plan_revision", "parent_plan_digest", "task_understanding_digest",
    "evidence_bundle_digest", "acceptance_contract_digest", "memory_snapshot_id",
    "planning_view_digest", "nodes", "critical_path", "budget"};

bool payload_fields(const json& payload, const std::set<std::string>& fields,
                    std::vector<ContractIssue>* issues) {
    return contracts::validate_object_fields(payload, fields, fields,
        contracts::UnknownFieldPolicy::Reject, nullptr, issues, "/payload");
}

json evidence_json(const EvidenceRecord& record) {
    return {{"evidence_id", record.evidence_id}, {"origin_kind", record.origin_kind},
            {"locator", record.locator}, {"content_digest", record.content_digest},
            {"collected_at", record.collected_at}, {"trust_class", record.trust_class},
            {"freshness_deadline", record.freshness_deadline},
            {"supported_claims", record.supported_claims},
            {"contradicted_claims", record.contradicted_claims},
            {"instruction_authority", record.instruction_authority}};
}

EvidenceRecord evidence_from_json(const json& value) {
    return {value.at("evidence_id").get<std::string>(),
            value.at("origin_kind").get<std::string>(),
            value.at("locator").get<std::string>(),
            value.at("content_digest").get<std::string>(),
            value.at("collected_at").get<std::string>(),
            value.at("trust_class").get<std::string>(),
            value.at("freshness_deadline").get<std::string>(),
            value.at("supported_claims").get<std::vector<std::string>>(),
            value.at("contradicted_claims").get<std::vector<std::string>>(),
            value.at("instruction_authority").get<bool>()};
}

json node_json(const PlanNode& node) {
    return {{"node_id", node.node_id}, {"objective", node.objective},
            {"in_scope", node.in_scope}, {"out_of_scope", node.out_of_scope},
            {"input_contracts", node.input_contracts},
            {"output_contracts", node.output_contracts},
            {"dependencies", node.dependencies},
            {"required_capabilities", node.required_capabilities},
            {"side_effects", node.side_effects},
            {"acceptance_contract_id", node.acceptance_contract_id},
            {"rollback_strategy", node.rollback_strategy},
            {"risk_level", node.risk_level},
            {"approval_required", node.approval_required}};
}

PlanNode node_from_json(const json& value) {
    PlanNode node;
    node.node_id = value.at("node_id").get<std::string>();
    node.objective = value.at("objective").get<std::string>();
    node.in_scope = value.at("in_scope").get<std::vector<std::string>>();
    node.out_of_scope = value.at("out_of_scope").get<std::vector<std::string>>();
    node.input_contracts = value.at("input_contracts").get<std::vector<std::string>>();
    node.output_contracts = value.at("output_contracts").get<std::vector<std::string>>();
    node.dependencies = value.at("dependencies").get<std::vector<std::string>>();
    node.required_capabilities = value.at("required_capabilities").get<std::vector<std::string>>();
    node.side_effects = value.at("side_effects").get<std::vector<std::string>>();
    node.acceptance_contract_id = value.at("acceptance_contract_id").get<std::string>();
    node.rollback_strategy = value.at("rollback_strategy").get<std::string>();
    node.risk_level = value.at("risk_level").get<std::string>();
    node.approval_required = value.at("approval_required").get<bool>();
    return node;
}

template <typename T, typename Builder>
std::optional<T> decode(const json& value, const char* kind,
                        const std::set<std::string>& fields,
                        const contracts::ParseContext& context,
                        std::vector<ContractIssue>* issues, Builder builder) {
    auto document = contracts::parse_typed_contract(value, kind, context, issues);
    if(!document || !payload_fields(document->payload, fields, issues)) return std::nullopt;
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

std::string to_string(ChangeMode mode) {
    switch(mode) {
        case ChangeMode::Repair: return "repair";
        case ChangeMode::Incremental: return "incremental";
        case ChangeMode::Refactor: return "refactor";
        case ChangeMode::Upgrade: return "upgrade";
        case ChangeMode::Transformation: return "transformation";
    }
    return "incremental";
}

std::optional<ChangeMode> change_mode_from_string(const std::string& value) {
    if(value == "repair") return ChangeMode::Repair;
    if(value == "incremental") return ChangeMode::Incremental;
    if(value == "refactor") return ChangeMode::Refactor;
    if(value == "upgrade") return ChangeMode::Upgrade;
    if(value == "transformation") return ChangeMode::Transformation;
    return std::nullopt;
}

json encode(const TaskIntake& value) {
    return contracts::make_typed_contract(value.metadata, "agent.task_intake/v1",
        {{"user_goal", value.user_goal},
         {"requested_deliverables", value.requested_deliverables},
         {"explicit_constraints", value.explicit_constraints},
         {"granted_authorities", value.granted_authorities},
         {"success_signals", value.success_signals}, {"fact_gaps", value.fact_gaps}});
}

json encode(const EvidenceBundle& value) {
    json records = json::array();
    for(const auto& record : value.records) records.push_back(evidence_json(record));
    return contracts::make_typed_contract(value.metadata, "agent.evidence_bundle/v1",
        {{"bundle_id", value.bundle_id}, {"records", std::move(records)}});
}

json encode(const TaskUnderstanding& value) {
    return contracts::make_typed_contract(value.metadata, "agent.task_understanding/v1",
        {{"domain", value.domain}, {"current_state", value.current_state},
         {"target_state", value.target_state}, {"gaps", value.gaps},
         {"assumptions", value.assumptions}, {"unknowns", value.unknowns},
         {"risks", value.risks}, {"evidence_ids", value.evidence_ids},
         {"change_mode", to_string(value.change_mode)}, {"blast_radius", value.blast_radius}});
}

json encode(const ExecutionPlan& value) {
    json nodes = json::array();
    for(const auto& node : value.nodes) nodes.push_back(node_json(node));
    return contracts::make_typed_contract(value.metadata, "agent.execution_plan/v1",
        {{"plan_revision", value.plan_revision}, {"parent_plan_digest", value.parent_plan_digest},
         {"task_understanding_digest", value.task_understanding_digest},
         {"evidence_bundle_digest", value.evidence_bundle_digest},
         {"acceptance_contract_digest", value.acceptance_contract_digest},
         {"memory_snapshot_id", value.memory_snapshot_id},
         {"planning_view_digest", value.planning_view_digest}, {"nodes", std::move(nodes)},
         {"critical_path", value.critical_path},
         {"budget", {{"wall_time_ms", value.budget.wall_time_ms},
                     {"token_budget", value.budget.token_budget},
                     {"tool_calls", value.budget.tool_calls},
                     {"cost_limit", value.budget.cost_limit}}}});
}

std::optional<TaskIntake> decode_task_intake(const json& value,
                                             const contracts::ParseContext& context,
                                             std::vector<ContractIssue>* issues) {
    return decode<TaskIntake>(value, "agent.task_intake/v1", kIntakeFields, context, issues,
        [](const json& p) {
            TaskIntake result;
            result.user_goal = p.at("user_goal").get<std::string>();
            result.requested_deliverables = p.at("requested_deliverables").get<std::vector<std::string>>();
            result.explicit_constraints = p.at("explicit_constraints").get<std::vector<std::string>>();
            result.granted_authorities = p.at("granted_authorities").get<std::vector<std::string>>();
            result.success_signals = p.at("success_signals").get<std::vector<std::string>>();
            result.fact_gaps = p.at("fact_gaps").get<std::vector<std::string>>();
            return result;
        });
}

std::optional<EvidenceBundle> decode_evidence_bundle(const json& value,
                                                     const contracts::ParseContext& context,
                                                     std::vector<ContractIssue>* issues) {
    return decode<EvidenceBundle>(value, "agent.evidence_bundle/v1", kEvidenceBundleFields,
        context, issues, [](const json& p) {
            EvidenceBundle result;
            result.bundle_id = p.at("bundle_id").get<std::string>();
            for(const auto& item : p.at("records")) result.records.push_back(evidence_from_json(item));
            return result;
        });
}

std::optional<TaskUnderstanding> decode_task_understanding(
    const json& value, const contracts::ParseContext& context,
    std::vector<ContractIssue>* issues) {
    return decode<TaskUnderstanding>(value, "agent.task_understanding/v1", kUnderstandingFields,
        context, issues, [](const json& p) {
            TaskUnderstanding result;
            result.domain = p.at("domain").get<std::string>();
            result.current_state = p.at("current_state").get<std::string>();
            result.target_state = p.at("target_state").get<std::string>();
            result.gaps = p.at("gaps").get<std::vector<std::string>>();
            result.assumptions = p.at("assumptions").get<std::vector<std::string>>();
            result.unknowns = p.at("unknowns").get<std::vector<std::string>>();
            result.risks = p.at("risks").get<std::vector<std::string>>();
            result.evidence_ids = p.at("evidence_ids").get<std::vector<std::string>>();
            const auto mode = change_mode_from_string(p.at("change_mode").get<std::string>());
            if(!mode) throw std::invalid_argument("unknown change_mode");
            result.change_mode = *mode;
            result.blast_radius = p.at("blast_radius").get<std::string>();
            return result;
        });
}

std::optional<ExecutionPlan> decode_execution_plan(
    const json& value, const contracts::ParseContext& context,
    std::vector<ContractIssue>* issues) {
    return decode<ExecutionPlan>(value, "agent.execution_plan/v1", kPlanFields,
        context, issues, [](const json& p) {
            ExecutionPlan result;
            result.plan_revision = p.at("plan_revision").get<std::uint64_t>();
            result.parent_plan_digest = p.at("parent_plan_digest").get<std::string>();
            result.task_understanding_digest = p.at("task_understanding_digest").get<std::string>();
            result.evidence_bundle_digest = p.at("evidence_bundle_digest").get<std::string>();
            result.acceptance_contract_digest = p.at("acceptance_contract_digest").get<std::string>();
            result.memory_snapshot_id = p.at("memory_snapshot_id").get<std::string>();
            result.planning_view_digest = p.at("planning_view_digest").get<std::string>();
            for(const auto& item : p.at("nodes")) result.nodes.push_back(node_from_json(item));
            result.critical_path = p.at("critical_path").get<std::vector<std::string>>();
            const auto& budget = p.at("budget");
            result.budget.wall_time_ms = budget.at("wall_time_ms").get<std::uint64_t>();
            result.budget.token_budget = budget.at("token_budget").get<std::uint64_t>();
            result.budget.tool_calls = budget.at("tool_calls").get<std::uint64_t>();
            result.budget.cost_limit = budget.at("cost_limit").get<double>();
            return result;
        });
}

}  // namespace agent_framework::planning
