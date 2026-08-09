#include "agent/remediation/remediation_workflow.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

namespace agent_framework::remediation {
namespace {
using json = nlohmann::json;

std::string default_now() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return stream.str();
}
std::string digest(const json& value) {
    const auto result = contracts::embedded_digest(value);
    if(!result) throw std::runtime_error("unable to compute canonical digest");
    return *result;
}
template <class T> void add(std::set<T>& target, const std::vector<T>& values) {
    target.insert(values.begin(), values.end());
}
template <class T> std::vector<T> vector_of(const std::set<T>& values) {
    return {values.begin(), values.end()};
}
bool contains(const std::vector<std::string>& values, std::string_view value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}
json memory_context(const memory_v2::MemoryView& view) {
    json records = json::array();
    for(const auto& record : view.records) records.push_back(memory_v2::encode(record));
    return {{"snapshot_id", view.snapshot.snapshot_id}, {"view_digest", view.manifest.view_digest},
            {"records", std::move(records)}, {"instruction_authority", false}};
}
bool semantic(RemediationStage stage) {
    return stage == RemediationStage::ImpactAnalysis ||
           stage == RemediationStage::RemediationPlanning ||
           stage == RemediationStage::ReverificationPlanning;
}
bool terminal(RemediationState state) {
    return state == RemediationState::ReadyForExecution || state == RemediationState::ManualReview ||
           state == RemediationState::Failed || state == RemediationState::Cancelled;
}
std::uint64_t tokens(const llm_runtime::LLMInvocationManifest& manifest) {
    return manifest.usage.input_tokens.value_or(0) + manifest.usage.output_tokens.value_or(0);
}
double cost(const llm_runtime::LLMInvocationManifest& manifest) {
    return manifest.usage.cost_usd.value_or(0.0);
}
bool output_fields(const json& output, const std::set<std::string>& fields) {
    return contracts::validate_object_fields(output, fields, fields,
        contracts::UnknownFieldPolicy::Reject, nullptr, nullptr, "/model_output");
}

bool validate_inventory(const planning::ExecutionPlan& plan,
                        const assurance::AcceptanceContract& contract,
                        const assurance::AcceptanceReport& report,
                        const ImpactInventory& inventory, std::string* error) {
    std::set<std::string> node_ids, criterion_ids, requirement_ids, artifact_ids, evidence_ids;
    for(const auto& node : plan.nodes) node_ids.insert(node.node_id);
    for(const auto& criterion : contract.criteria) criterion_ids.insert(criterion.criterion_id);
    for(const auto& requirement : inventory.requirements)
        if(requirement.requirement_id.empty() || !requirement_ids.insert(requirement.requirement_id).second) {
            if(error) *error = "impact inventory requirement ids must be non-empty and unique";
            return false;
        }
    for(const auto& artifact : inventory.artifacts)
        if(artifact.artifact_id.empty() || !artifact_ids.insert(artifact.artifact_id).second ||
           artifact.content_digest.rfind("sha256:", 0) != 0) {
            if(error) *error = "impact inventory artifact ids/digests are invalid";
            return false;
        }
    for(const auto& item : inventory.evidence)
        if(item.evidence_id.empty() || !evidence_ids.insert(item.evidence_id).second ||
           item.content_digest.rfind("sha256:", 0) != 0) {
            if(error) *error = "impact inventory evidence ids/digests are invalid";
            return false;
        }
    for(const auto& requirement : inventory.requirements) {
        for(const auto& id : requirement.criterion_ids) if(!criterion_ids.count(id)) {
            if(error) *error = "requirement references an unknown criterion";
            return false;
        }
        for(const auto& id : requirement.plan_node_ids) if(!node_ids.count(id)) {
            if(error) *error = "requirement references an unknown plan node";
            return false;
        }
        for(const auto& id : requirement.artifact_ids) if(!artifact_ids.count(id)) {
            if(error) *error = "requirement references an unknown artifact";
            return false;
        }
    }
    for(const auto& artifact : inventory.artifacts) {
        if(!artifact.producer_node_id.empty() && !node_ids.count(artifact.producer_node_id)) {
            if(error) *error = "artifact references an unknown producer node";
            return false;
        }
        for(const auto& id : artifact.criterion_ids) if(!criterion_ids.count(id)) {
            if(error) *error = "artifact references an unknown criterion";
            return false;
        }
        for(const auto& id : artifact.depends_on_artifact_ids) if(!artifact_ids.count(id)) {
            if(error) *error = "artifact dependency references an unknown artifact";
            return false;
        }
        for(const auto& id : artifact.evidence_ids) if(!evidence_ids.count(id)) {
            if(error) *error = "artifact references unknown evidence";
            return false;
        }
    }
    for(const auto& item : inventory.evidence)
        if(!criterion_ids.count(item.criterion_id) ||
           (!item.artifact_id.empty() && !artifact_ids.count(item.artifact_id))) {
            if(error) *error = "evidence references an unknown criterion or artifact";
            return false;
        }
    std::set<std::string> finding_ids;
    for(const auto& finding : report.findings) {
        const auto id = finding.finding_id.empty() ? "finding:" + finding.criterion_id : finding.finding_id;
        if(!criterion_ids.count(finding.criterion_id) || !finding_ids.insert(id).second) {
            if(error) *error = "report finding ids/criteria must be known and unique";
            return false;
        }
    }
    return true;
}

std::optional<ImpactGraph> build_impact_graph(
    const json& output, const assurance::AcceptanceReport& report,
    const ImpactInventory& inventory, std::string_view inventory_digest,
    std::string_view report_digest, std::string_view workflow_id, std::string* error) {
    static const std::set<std::string> fields = {
        "additional_plan_node_ids", "additional_artifact_ids", "rationale"};
    if(!output.is_object() || !output_fields(output, fields)) {
        if(error) *error = "impact analyst output has an invalid closed schema";
        return std::nullopt;
    }
    ImpactGraph graph;
    graph.metadata = inventory.metadata;
    graph.graph_id = std::string(workflow_id) + ":impact";
    graph.inventory_digest = std::string(inventory_digest);
    graph.acceptance_report_digest = std::string(report_digest);
    std::set<std::string> criteria, nodes, artifacts, evidence, memory, roles;
    std::set<std::string> known_nodes, known_artifacts;
    for(const auto& requirement : inventory.requirements) add(known_nodes, requirement.plan_node_ids);
    for(const auto& artifact : inventory.artifacts) {
        known_artifacts.insert(artifact.artifact_id);
        if(!artifact.producer_node_id.empty()) known_nodes.insert(artifact.producer_node_id);
    }
    try {
        for(const auto& id : output.at("additional_plan_node_ids").get<std::vector<std::string>>()) {
            if(!known_nodes.count(id)) throw std::invalid_argument("impact analyst referenced an unknown plan node");
            nodes.insert(id);
        }
        for(const auto& id : output.at("additional_artifact_ids").get<std::vector<std::string>>()) {
            if(!known_artifacts.count(id)) throw std::invalid_argument("impact analyst referenced an unknown artifact");
            artifacts.insert(id);
        }
    } catch(const std::exception& e) {
        if(error) *error = e.what();
        return std::nullopt;
    }
    for(const auto& finding : report.findings) {
        if(finding.outcome == assurance::FindingOutcome::Pass) continue;
        FindingBinding binding;
        binding.finding_id = finding.finding_id.empty()
            ? "finding:" + finding.criterion_id : finding.finding_id;
        binding.criterion_id = finding.criterion_id;
        criteria.insert(finding.criterion_id);
        for(const auto& requirement : inventory.requirements) {
            if(!contains(requirement.criterion_ids, finding.criterion_id)) continue;
            binding.requirement_ids.push_back(requirement.requirement_id);
            add(nodes, requirement.plan_node_ids);
            add(artifacts, requirement.artifact_ids);
            binding.plan_node_ids.insert(binding.plan_node_ids.end(),
                                         requirement.plan_node_ids.begin(), requirement.plan_node_ids.end());
            binding.artifact_ids.insert(binding.artifact_ids.end(),
                                        requirement.artifact_ids.begin(), requirement.artifact_ids.end());
        }
        for(const auto& artifact : inventory.artifacts) {
            if(contains(artifact.criterion_ids, finding.criterion_id)) {
                artifacts.insert(artifact.artifact_id);
                binding.artifact_ids.push_back(artifact.artifact_id);
                if(!artifact.producer_node_id.empty()) {
                    nodes.insert(artifact.producer_node_id);
                    binding.plan_node_ids.push_back(artifact.producer_node_id);
                }
            }
        }
        for(const auto& item : inventory.evidence) {
            if(item.criterion_id == finding.criterion_id || contains(finding.evidence_ids, item.evidence_id)) {
                evidence.insert(item.evidence_id);
                binding.evidence_ids.push_back(item.evidence_id);
                if(!item.artifact_id.empty()) artifacts.insert(item.artifact_id);
            }
        }
        auto unique = [](std::vector<std::string>& v) {
            std::sort(v.begin(), v.end()); v.erase(std::unique(v.begin(), v.end()), v.end());
        };
        unique(binding.requirement_ids); unique(binding.plan_node_ids);
        unique(binding.artifact_ids); unique(binding.evidence_ids);
        if(binding.plan_node_ids.empty() || binding.artifact_ids.empty()) {
            if(error) *error = "each non-pass finding must map to a plan node and artifact";
            return std::nullopt;
        }
        graph.finding_bindings.push_back(std::move(binding));
    }
    bool changed = true;
    while(changed) {
        changed = false;
        for(const auto& artifact : inventory.artifacts) {
            if(artifacts.count(artifact.artifact_id)) continue;
            if(std::any_of(artifact.depends_on_artifact_ids.begin(), artifact.depends_on_artifact_ids.end(),
                           [&](const auto& id) { return artifacts.count(id) != 0; })) {
                artifacts.insert(artifact.artifact_id); changed = true;
            }
        }
    }
    for(const auto& artifact : inventory.artifacts) {
        if(!artifacts.count(artifact.artifact_id)) continue;
        if(!artifact.producer_node_id.empty()) nodes.insert(artifact.producer_node_id);
        add(evidence, artifact.evidence_ids); add(memory, artifact.memory_record_ids);
        add(roles, artifact.verifier_roles);
    }
    for(const auto& item : inventory.evidence)
        if(criteria.count(item.criterion_id) || artifacts.count(item.artifact_id)) evidence.insert(item.evidence_id);
    graph.affected_criterion_ids = vector_of(criteria);
    graph.affected_plan_node_ids = vector_of(nodes);
    graph.invalidated_artifact_ids = vector_of(artifacts);
    graph.invalidated_evidence_ids = vector_of(evidence);
    graph.invalidated_memory_record_ids = vector_of(memory);
    graph.verifier_roles = vector_of(roles);
    if(graph.finding_bindings.empty() || graph.affected_plan_node_ids.empty() ||
       graph.invalidated_artifact_ids.empty()) {
        if(error) *error = "every non-pass finding must map to a plan node and artifact";
        return std::nullopt;
    }
    return graph;
}

std::optional<RemediationPlan> parse_remediation_plan(
    const json& output, const planning::ExecutionPlan& current,
    const assurance::AcceptanceContract& contract,
    const ImpactGraph& impact, const RemediationWorkflowOptions& options,
    std::string_view workflow_id, std::string_view report_digest,
    std::string_view impact_digest, std::string_view invocation_id, std::string* error) {
    static const std::set<std::string> fields = {"actions", "criterion_changes"};
    if(!output.is_object() || !output_fields(output, fields) || !output.at("actions").is_array() ||
       !output.at("criterion_changes").is_array()) {
        if(error) *error = "remediation planner output has an invalid closed schema";
        return std::nullopt;
    }
    std::set<std::string> finding_ids, affected_nodes(impact.affected_plan_node_ids.begin(),
        impact.affected_plan_node_ids.end()), affected_artifacts(impact.invalidated_artifact_ids.begin(),
        impact.invalidated_artifact_ids.end()), capabilities(options.allowed_capabilities.begin(),
        options.allowed_capabilities.end()), action_ids, covered_findings;
    for(const auto& binding : impact.finding_bindings) finding_ids.insert(binding.finding_id);
    RemediationPlan plan;
    plan.metadata = current.metadata; plan.remediation_id = std::string(workflow_id) + ":plan";
    plan.parent_plan_digest = digest(planning::encode(current));
    plan.acceptance_report_digest = std::string(report_digest);
    plan.impact_graph_digest = std::string(impact_digest);
    plan.planner_invocation_id = std::string(invocation_id);
    try {
        for(const auto& item : output.at("actions")) {
            static const std::set<std::string> af = {"action_id", "objective", "finding_ids",
                "affected_plan_node_ids", "affected_artifact_ids", "required_capabilities",
                "side_effects", "output_contracts", "rollback_strategy", "risk_level",
                "approval_required"};
            if(!output_fields(item, af)) throw std::invalid_argument("invalid remediation action fields");
            RemediationAction action{item.at("action_id").get<std::string>(),
                item.at("objective").get<std::string>(), item.at("finding_ids").get<std::vector<std::string>>(),
                item.at("affected_plan_node_ids").get<std::vector<std::string>>(),
                item.at("affected_artifact_ids").get<std::vector<std::string>>(),
                item.at("required_capabilities").get<std::vector<std::string>>(),
                item.at("side_effects").get<std::vector<std::string>>(),
                item.at("output_contracts").get<std::vector<std::string>>(),
                item.at("rollback_strategy").get<std::string>(), item.at("risk_level").get<std::string>(),
                item.at("approval_required").get<bool>()};
            if(action.action_id.empty() || !action_ids.insert(action.action_id).second ||
               action.objective.empty() || action.output_contracts.empty())
                throw std::invalid_argument("action identity, objective and output contracts are required");
            for(const auto& id : action.finding_ids) {
                if(!finding_ids.count(id)) throw std::invalid_argument("action references an unknown finding");
                covered_findings.insert(id);
            }
            for(const auto& id : action.affected_plan_node_ids)
                if(!affected_nodes.count(id)) throw std::invalid_argument("action exceeds the impact plan-node closure");
            for(const auto& id : action.affected_artifact_ids)
                if(!affected_artifacts.count(id)) throw std::invalid_argument("action exceeds the impact artifact closure");
            for(const auto& cap : action.required_capabilities)
                if(!capabilities.count(cap)) throw std::invalid_argument("action requests an unauthorized capability");
            if(!action.side_effects.empty() && action.rollback_strategy.empty())
                throw std::invalid_argument("side-effecting action requires rollback");
            if((action.risk_level == "high" || action.risk_level == "critical") && !action.approval_required)
                throw std::invalid_argument("high-risk action must require approval");
            plan.actions.push_back(std::move(action));
        }
        if(plan.actions.empty() || covered_findings != finding_ids)
            throw std::invalid_argument("remediation actions must cover every non-pass finding");
        for(const auto& item : output.at("criterion_changes")) {
            static const std::set<std::string> cf = {"criterion_id", "new_mandatory", "new_threshold", "rationale"};
            if(!output_fields(item, cf)) throw std::invalid_argument("invalid criterion change fields");
            const auto id = item.at("criterion_id").get<std::string>();
            const auto found = std::find_if(contract.criteria.begin(), contract.criteria.end(),
                [&](const auto& c) { return c.criterion_id == id; });
            if(found == contract.criteria.end()) throw std::invalid_argument("criterion change references unknown criterion");
            plan.criterion_changes.push_back({id, found->mandatory, item.at("new_mandatory").get<bool>(),
                found->threshold, item.at("new_threshold").get<std::string>(),
                item.at("rationale").get<std::string>()});
        }
    } catch(const std::exception& e) {
        if(error) *error = e.what();
        return std::nullopt;
    }
    return plan;
}

planning::ExecutionPlan make_revised_plan(
    const planning::ExecutionPlan& current, const assurance::AcceptanceContract& contract,
    const RemediationPlan& remediation) {
    auto revised = current;
    revised.plan_revision = current.plan_revision + 1;
    revised.parent_plan_digest = digest(planning::encode(current));
    auto revised_contract = contract;
    if(!remediation.criterion_changes.empty()) {
        revised_contract.revision++;
        for(const auto& change : remediation.criterion_changes)
            for(auto& criterion : revised_contract.criteria)
                if(criterion.criterion_id == change.criterion_id) {
                    criterion.mandatory = change.new_mandatory;
                    criterion.threshold = change.new_threshold;
                }
        revised.acceptance_contract_digest = digest(assurance::encode(revised_contract));
    }
    for(const auto& action : remediation.actions) {
        planning::PlanNode node;
        node.node_id = "remediation:" + action.action_id;
        node.objective = action.objective;
        node.in_scope = action.affected_artifact_ids;
        node.input_contracts = action.affected_artifact_ids;
        node.output_contracts = action.output_contracts;
        node.dependencies = action.affected_plan_node_ids;
        node.required_capabilities = action.required_capabilities;
        node.side_effects = action.side_effects;
        node.acceptance_contract_id = remediation.criterion_changes.empty()
            ? contract.criteria.front().criterion_id : remediation.criterion_changes.front().criterion_id;
        node.rollback_strategy = action.rollback_strategy;
        node.risk_level = action.risk_level;
        node.approval_required = action.approval_required;
        revised.nodes.push_back(std::move(node));
        revised.critical_path.push_back("remediation:" + action.action_id);
    }
    return revised;
}

std::optional<ReverificationPlan> build_reverification_plan(
    const json& output, const assurance::AcceptanceContract& contract,
    const ImpactInventory& inventory, const ImpactGraph& impact,
    const RemediationPlan& remediation, const planning::ExecutionPlan& proposed,
    std::string_view workflow_id, std::string_view now, std::string* error) {
    static const std::set<std::string> fields = {"criterion_ids", "reuse_evidence_ids", "verifier_roles"};
    if(!output.is_object() || !output_fields(output, fields)) {
        if(error) *error = "reverification planner output has an invalid closed schema";
        return std::nullopt;
    }
    std::set<std::string> criteria(impact.affected_criterion_ids.begin(), impact.affected_criterion_ids.end());
    std::set<std::string> roles(impact.verifier_roles.begin(), impact.verifier_roles.end());
    std::set<std::string> invalid(impact.invalidated_evidence_ids.begin(), impact.invalidated_evidence_ids.end());
    std::set<std::string> known_criteria, known_roles;
    for(const auto& c : contract.criteria) known_criteria.insert(c.criterion_id);
    for(const auto& a : inventory.artifacts) add(known_roles, a.verifier_roles);
    try {
        for(const auto& id : output.at("criterion_ids").get<std::vector<std::string>>()) {
            if(!known_criteria.count(id)) throw std::invalid_argument("unknown reverification criterion");
            criteria.insert(id);
        }
        for(const auto& role : output.at("verifier_roles").get<std::vector<std::string>>()) {
            if(!known_roles.count(role)) throw std::invalid_argument("unknown reverification role");
            roles.insert(role);
        }
    } catch(const std::exception& e) {
        if(error) *error = e.what();
        return std::nullopt;
    }
    ReverificationPlan plan;
    plan.metadata = contract.metadata;
    plan.reverification_id = std::string(workflow_id) + ":reverify";
    plan.remediation_plan_digest = digest(encode(remediation));
    plan.proposed_plan_digest = digest(planning::encode(proposed));
    plan.criterion_ids = vector_of(criteria); plan.verifier_roles = vector_of(roles);
    plan.invalidated_evidence_ids = vector_of(invalid); plan.created_at = std::string(now);
    std::set<std::string> oracle_kinds;
    for(const auto& criterion : contract.criteria)
        if(criteria.count(criterion.criterion_id)) add(oracle_kinds, criterion.required_evidence);
    for(const auto& evidence : inventory.evidence)
        if(invalid.count(evidence.evidence_id) && evidence.strong_oracle) oracle_kinds.insert(evidence.source_kind);
    plan.forced_oracle_kinds = vector_of(oracle_kinds);
    std::set<std::string> requested;
    try { add(requested, output.at("reuse_evidence_ids").get<std::vector<std::string>>()); }
    catch(const std::exception& e) { if(error) *error = e.what(); return std::nullopt; }
    for(const auto& item : inventory.evidence) {
        if(!requested.count(item.evidence_id)) continue;
        if(invalid.count(item.evidence_id) || item.content_digest.rfind("sha256:", 0) != 0 ||
           item.freshness_deadline.empty() || item.freshness_deadline < now) continue;
        plan.reusable_evidence_ids.push_back(item.evidence_id);
    }
    for(const auto& artifact : inventory.artifacts) {
        if(contains(impact.invalidated_artifact_ids, artifact.artifact_id)) continue;
        plan.baseline_artifact_digests[artifact.artifact_id] = artifact.content_digest;
    }
    return plan;
}
}  // namespace

RoleRuntimeRemediationModel::RoleRuntimeRemediationModel(
    std::shared_ptr<llm_runtime::RoleRuntime> runtime) : runtime_(std::move(runtime)) {
    if(!runtime_) throw std::invalid_argument("RoleRuntime is required");
}
bool RoleRuntimeRemediationModel::bind(RemediationStage stage, RemediationRoleBinding binding) {
    if(!semantic(stage) || binding.profile_id.empty() || binding.profile_revision.empty()) return false;
    return bindings_.emplace(stage, std::move(binding)).second;
}
RemediationStageResponse RoleRuntimeRemediationModel::invoke(const RemediationStageRequest& request) {
    RemediationStageResponse result;
    if(request.cancelled && request.cancelled()) {
        result.error_code = "remediation_cancelled"; result.error_message = "cancelled before invocation";
        return result;
    }
    const auto binding = bindings_.find(request.stage);
    if(binding == bindings_.end()) {
        result.error_code = "remediation_role_unbound";
        result.error_message = "no RoleRuntime profile is bound to " + remediation_stage_name(request.stage);
        return result;
    }
    const auto input = contracts::canonical_json(request.input);
    llm_runtime::RoleInvocationRequest invocation;
    invocation.metadata = request.metadata;
    invocation.invocation_id = request.workflow_id + ":" + remediation_stage_name(request.stage) +
                               ":" + std::to_string(request.attempt);
    invocation.trace_id = request.metadata.identity.run_id.empty() ? request.workflow_id
                                                                   : request.metadata.identity.run_id;
    invocation.profile_id = binding->second.profile_id;
    invocation.profile_revision = binding->second.profile_revision;
    invocation.prompt_variables = {{"input", input}};
    invocation.input.context = memory_context(request.memory_view).dump();
    invocation.memory_view = {request.memory_view.snapshot.snapshot_id, "replan",
                              request.memory_view.manifest.view_digest};
    for(const auto& capability : binding->second.granted_capabilities)
        if(contains(request.granted_capabilities, capability)) invocation.granted_capabilities.push_back(capability);
    invocation.independence = request.independence;
    invocation.required_region = binding->second.required_region;
    invocation.estimated_input_tokens = (input.size() + invocation.input.context.size() + 3) / 4;
    invocation.policy_revision = "phase4-v2-f5r-r1";
    auto runtime_result = runtime_->invoke(std::move(invocation));
    result.manifest = runtime_result.manifest;
    result.error_code = runtime_result.error_code; result.error_message = runtime_result.error_message;
    if(!runtime_result.ok || !runtime_result.structured_output) return result;
    result.ok = true; result.output = *runtime_result.structured_output;
    return result;
}

LLMRemediationWorkflow::LLMRemediationWorkflow(
    memory_v2::MemoryViewEngine& views, RemediationStore& store, planning::PlanStore& plans,
    RemediationStageModel& model, approval::PolicyDecisionPoint policy)
    : views_(views), store_(store), plans_(plans), model_(model), policy_(std::move(policy)) {}

RemediationWorkflowResult LLMRemediationWorkflow::run(
    const planning::ExecutionPlan& current_plan, const assurance::AcceptanceContract& contract,
    const assurance::AcceptanceReport& report,
    const assurance::AssuranceCheckpoint& assurance_checkpoint,
    const ImpactInventory& inventory, const memory_v2::MemoryScope& subject,
    const RemediationWorkflowOptions& options) {
    RemediationWorkflowResult result;
    const auto now = options.now ? options.now : default_now;
    const auto current_digest = digest(planning::encode(current_plan));
    const auto contract_digest = digest(assurance::encode(contract));
    const auto report_digest = digest(assurance::encode(report));
    const auto assurance_digest = digest(assurance::encode(assurance_checkpoint));
    const auto inventory_digest = digest(encode(inventory));
    const auto workflow_id = options.workflow_id.empty()
        ? current_plan.metadata.identity.task_id + ":remediation" : options.workflow_id;
    auto finish = [&](RemediationCheckpoint checkpoint) {
        result.state = checkpoint.state; result.error_code = checkpoint.error_code;
        result.error_message = checkpoint.error_message; result.impact_graph = checkpoint.impact_graph;
        result.remediation_plan = checkpoint.remediation_plan; result.proposed_plan = checkpoint.proposed_plan;
        result.reverification_plan = checkpoint.reverification_plan; result.checkpoint = std::move(checkpoint);
        return result;
    };
    std::string inventory_error;
    if(current_plan.metadata.identity.tenant_id.empty() || current_plan.metadata.identity.task_id.empty() ||
       contract.metadata.identity.tenant_id != current_plan.metadata.identity.tenant_id ||
       report.metadata.identity.task_id != current_plan.metadata.identity.task_id ||
       inventory.metadata.identity.task_id != current_plan.metadata.identity.task_id ||
       inventory.plan_digest != current_digest || report.plan_digest != current_digest ||
       report.acceptance_contract_digest != contract_digest ||
       assurance_checkpoint.acceptance_contract_digest != contract_digest ||
       assurance_checkpoint.acceptance_report_digest != report_digest ||
       inventory.artifact_manifest_digest != assurance_checkpoint.artifact_manifest_digest ||
       subject.tenant_id != current_plan.metadata.identity.tenant_id ||
       !validate_inventory(current_plan, contract, report, inventory, &inventory_error)) {
        result.error_code = "remediation_input_invalid";
        result.error_message = inventory_error.empty()
            ? "identity and plan/report/assurance/inventory digest bindings must agree"
            : inventory_error;
        return result;
    }
    auto view = views_.build(memory_v2::make_view_spec(memory_v2::MemoryViewMode::Replan,
        current_plan.metadata, subject), now());
    if(view.fail_closed) {
        result.state = RemediationState::ManualReview; result.error_code = "replan_view_failed";
        result.error_message = view.error; return result;
    }
    auto stored = store_.load(current_plan.metadata.identity.tenant_id, workflow_id);
    RemediationCheckpoint checkpoint;
    std::uint64_t store_revision = 0;
    if(stored) {
        checkpoint = stored->checkpoint; store_revision = stored->revision;
        if(checkpoint.current_plan_digest != current_digest ||
           checkpoint.acceptance_contract_digest != contract_digest ||
           checkpoint.acceptance_report_digest != report_digest ||
           checkpoint.assurance_checkpoint_digest != assurance_digest ||
           checkpoint.impact_inventory_digest != inventory_digest ||
           checkpoint.memory_snapshot_id != view.snapshot.snapshot_id ||
           checkpoint.memory_view_digest != view.manifest.view_digest) {
            checkpoint.state = RemediationState::ManualReview;
            checkpoint.error_code = "remediation_input_digest_mismatch";
            checkpoint.error_message = "durable remediation inputs or pinned Replan Memory View changed";
            return finish(std::move(checkpoint));
        }
        if(terminal(checkpoint.state)) return finish(std::move(checkpoint));
    } else {
        checkpoint.metadata = current_plan.metadata; checkpoint.workflow_id = workflow_id;
        checkpoint.current_plan_digest = current_digest; checkpoint.acceptance_contract_digest = contract_digest;
        checkpoint.acceptance_report_digest = report_digest; checkpoint.assurance_checkpoint_digest = assurance_digest;
        checkpoint.impact_inventory_digest = inventory_digest; checkpoint.memory_snapshot_id = view.snapshot.snapshot_id;
        checkpoint.memory_view_digest = view.manifest.view_digest; checkpoint.updated_at = now();
        auto commit = store_.create(checkpoint);
        if(!commit) { result.error_code = "remediation_checkpoint_create_failed"; result.error_message = commit.error; return result; }
        store_revision = commit.revision;
    }
    auto persist = [&] {
        checkpoint.updated_at = now(); checkpoint.revision = store_revision + 1;
        auto commit = store_.compare_exchange(checkpoint, store_revision);
        if(!commit) throw std::runtime_error("remediation checkpoint CAS failed: " + commit.error);
        store_revision = commit.revision;
    };
    auto complete_stage = [&](RemediationStage stage, RemediationStage next) {
        checkpoint.completed_stages.push_back(remediation_stage_name(stage));
        checkpoint.next_stage = next; persist();
    };
    auto manual = [&](std::string code, std::string message) {
        checkpoint.state = RemediationState::ManualReview; checkpoint.error_code = std::move(code);
        checkpoint.error_message = std::move(message); persist(); return finish(checkpoint);
    };
    auto cancelled = [&] {
        if(!options.cancelled || !options.cancelled()) return false;
        checkpoint.state = RemediationState::Cancelled; checkpoint.error_code = "remediation_cancelled";
        checkpoint.error_message = "remediation workflow cancelled"; persist(); return true;
    };
    auto invoke = [&](RemediationStage stage, json input) -> std::optional<RemediationStageResponse> {
        const auto key = remediation_stage_name(stage);
        auto& attempt = checkpoint.stage_attempts[key];
        if(attempt >= options.max_iterations) return std::nullopt;
        ++attempt; persist();
        RemediationStageRequest request{current_plan.metadata, workflow_id, stage, attempt,
            view, std::move(input), {}, options.allowed_capabilities, options.cancelled};
        auto response = model_.invoke(request);
        if(response.ok) {
            const auto used_tokens = tokens(response.manifest); const auto used_cost = cost(response.manifest);
            checkpoint.consumed_tokens += used_tokens; checkpoint.consumed_cost_usd += used_cost;
            checkpoint.artifacts.push_back({stage, attempt, response.manifest.invocation_id,
                response.manifest.output_digest.empty() ? digest(response.output) : response.manifest.output_digest,
                response.manifest.provider, response.manifest.model, used_tokens, used_cost, response.output});
            if(checkpoint.consumed_tokens > options.token_budget ||
               checkpoint.consumed_cost_usd > options.cost_limit_usd ||
               (!options.deadline.empty() && now() > options.deadline)) return std::nullopt;
        }
        return response;
    };
    try {
        while(true) {
            if(cancelled()) return finish(checkpoint);
            if(!options.deadline.empty() && now() > options.deadline)
                return manual("remediation_budget_exhausted", "remediation deadline expired");
            if(checkpoint.next_stage == RemediationStage::ImpactAnalysis) {
                auto response = invoke(RemediationStage::ImpactAnalysis,
                    {{"acceptance_report", assurance::encode(report)}, {"impact_inventory", encode(inventory)},
                     {"rule", "recommend only inventory-backed additions; deterministic closure is authoritative"}});
                if(!response) return manual("remediation_budget_exhausted", "impact analysis iteration/token/cost budget exhausted");
                if(!response->ok) {
                    if(checkpoint.stage_attempts["impact_analysis"] >= options.max_iterations)
                        return manual(response->error_code.empty() ? "impact_analysis_failed" : response->error_code,
                                      response->error_message);
                    continue;
                }
                std::string error;
                auto graph = build_impact_graph(response->output, report, inventory, inventory_digest,
                    report_digest, workflow_id, &error);
                if(!graph) {
                    if(checkpoint.stage_attempts["impact_analysis"] >= options.max_iterations)
                        return manual("impact_mapping_invalid", error);
                    continue;
                }
                checkpoint.impact_graph = std::move(*graph);
                complete_stage(RemediationStage::ImpactAnalysis, RemediationStage::RemediationPlanning);
                continue;
            }
            if(checkpoint.next_stage == RemediationStage::RemediationPlanning) {
                auto response = invoke(RemediationStage::RemediationPlanning,
                    {{"current_plan", planning::encode(current_plan)}, {"acceptance_contract", assurance::encode(contract)},
                     {"impact_graph", encode(*checkpoint.impact_graph)},
                     {"rule", "cover every finding; stay within impact closure; declare all criterion changes"}});
                if(!response) return manual("remediation_budget_exhausted", "planner iteration/token/cost budget exhausted");
                if(!response->ok) {
                    if(checkpoint.stage_attempts["remediation_planning"] >= options.max_iterations)
                        return manual(response->error_code.empty() ? "remediation_planner_failed" : response->error_code,
                                      response->error_message);
                    continue;
                }
                std::string error;
                const auto impact_digest = digest(encode(*checkpoint.impact_graph));
                auto plan = parse_remediation_plan(response->output, current_plan, contract,
                    *checkpoint.impact_graph, options, workflow_id, report_digest, impact_digest,
                    response->manifest.invocation_id, &error);
                if(!plan) {
                    if(checkpoint.stage_attempts["remediation_planning"] >= options.max_iterations)
                        return manual("remediation_plan_invalid", error);
                    continue;
                }
                const auto signature = digest(json{{"actions", response->output.at("actions")},
                                                    {"criterion_changes", response->output.at("criterion_changes")}});
                if(contains(checkpoint.proposal_signatures, signature))
                    return manual("remediation_loop_detected", "a semantically identical remediation proposal repeated");
                checkpoint.proposal_signatures.push_back(signature);
                checkpoint.remediation_plan = std::move(*plan);
                checkpoint.proposed_plan = make_revised_plan(current_plan, contract, *checkpoint.remediation_plan);
                planning::PlanValidator validator;
                const auto validation = validator.validate(*checkpoint.proposed_plan);
                if(!validation.valid()) return manual("revised_plan_invalid", validation.issues.front().message);
                complete_stage(RemediationStage::RemediationPlanning, RemediationStage::PolicyGate);
                continue;
            }
            if(checkpoint.next_stage == RemediationStage::PolicyGate) {
                bool criterion_change = !checkpoint.remediation_plan->criterion_changes.empty();
                bool action_approval = false;
                for(const auto& action : checkpoint.remediation_plan->actions) {
                    approval::PolicyContext context;
                    context.identity = current_plan.metadata.identity;
                    context.actor_id = options.actor_id.empty() ? "remediation.workflow" : options.actor_id;
                    context.actor_roles.insert(options.actor_roles.begin(), options.actor_roles.end());
                    context.action = "plan.remediation"; context.resource = action.action_id;
                    context.effect_class = action.side_effects.empty() ? "none" : "workspace_write";
                    context.risk_level = action.risk_level; context.requester_id = context.actor_id;
                    const auto evaluation = policy_.evaluate(context);
                    if(evaluation.outcome == approval::PolicyOutcome::Deny)
                        return manual("remediation_policy_denied", evaluation.reasons.empty() ? "policy denied remediation" : evaluation.reasons.front());
                    action_approval = action_approval || action.approval_required ||
                                      evaluation.outcome == approval::PolicyOutcome::RequireApproval;
                }
                checkpoint.approval_request_digest = digest(json{
                    {"remediation_plan_digest", digest(encode(*checkpoint.remediation_plan))},
                    {"proposed_plan_digest", digest(planning::encode(*checkpoint.proposed_plan))},
                    {"criterion_change", criterion_change}, {"action_approval", action_approval},
                    {"policy_revision", policy_.rules().revision}});
                if(criterion_change || action_approval) {
                    if(options.approval_decision_id.empty() || !options.approval_validator ||
                       !options.approval_validator(checkpoint.approval_request_digest,
                                                   options.approval_decision_id)) {
                        checkpoint.state = RemediationState::AwaitingApproval;
                        checkpoint.error_code = "remediation_approval_required";
                        checkpoint.error_message = "criterion changes or governed actions require a bound approval decision";
                        persist(); return finish(checkpoint);
                    }
                    checkpoint.approval_decision_id = options.approval_decision_id;
                }
                checkpoint.state = RemediationState::Running; checkpoint.error_code.clear(); checkpoint.error_message.clear();
                complete_stage(RemediationStage::PolicyGate, RemediationStage::PlanCommit);
                continue;
            }
            if(checkpoint.next_stage == RemediationStage::PlanCommit) {
                const auto proposed_digest = digest(planning::encode(*checkpoint.proposed_plan));
                const auto live = plans_.current(current_plan.metadata.identity);
                if(!live) return manual("plan_store_missing", "current execution plan is absent");
                const auto live_digest = digest(planning::encode(*live));
                if(live_digest != proposed_digest) {
                    if(live_digest != current_digest || live->plan_revision != current_plan.plan_revision)
                        return manual("plan_revision_conflict", "another writer changed the execution plan");
                    const auto commit = plans_.compare_exchange(*checkpoint.proposed_plan,
                                                                current_plan.plan_revision);
                    if(commit.status != planning::PlanningCommitStatus::Committed)
                        return manual("plan_commit_failed", commit.error);
                    checkpoint.committed_plan_digest = commit.digest;
                    if(options.after_plan_commit) options.after_plan_commit();
                } else checkpoint.committed_plan_digest = proposed_digest;
                complete_stage(RemediationStage::PlanCommit, RemediationStage::ReverificationPlanning);
                continue;
            }
            if(checkpoint.next_stage == RemediationStage::ReverificationPlanning) {
                auto response = invoke(RemediationStage::ReverificationPlanning,
                    {{"impact_graph", encode(*checkpoint.impact_graph)},
                     {"remediation_plan", encode(*checkpoint.remediation_plan)},
                     {"inventory", encode(inventory)},
                     {"rule", "invalidated or stale evidence cannot be reused; strong oracles are rerun"}});
                if(!response) return manual("remediation_budget_exhausted", "reverification planning budget exhausted");
                if(!response->ok) {
                    if(checkpoint.stage_attempts["reverification_planning"] >= options.max_iterations)
                        return manual(response->error_code.empty() ? "reverification_planner_failed" : response->error_code,
                                      response->error_message);
                    continue;
                }
                std::string error;
                auto plan = build_reverification_plan(response->output, contract, inventory,
                    *checkpoint.impact_graph, *checkpoint.remediation_plan, *checkpoint.proposed_plan,
                    workflow_id, now(), &error);
                if(!plan) {
                    if(checkpoint.stage_attempts["reverification_planning"] >= options.max_iterations)
                        return manual("reverification_plan_invalid", error);
                    continue;
                }
                checkpoint.reverification_plan = std::move(*plan);
                complete_stage(RemediationStage::ReverificationPlanning, RemediationStage::Complete);
                continue;
            }
            checkpoint.state = RemediationState::ReadyForExecution;
            checkpoint.error_code.clear(); checkpoint.error_message.clear(); persist();
            return finish(checkpoint);
        }
    } catch(const std::exception& e) {
        result.state = RemediationState::Failed; result.checkpoint = checkpoint;
        result.error_code = "remediation_interrupted"; result.error_message = e.what();
        return result;
    }
}

}  // namespace agent_framework::remediation
