#include "agent/planning/cognition_pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <set>
#include <stdexcept>
#include <utility>

#include "agent/observability/audit.hpp"

namespace agent_framework::planning {
namespace {

using json = nlohmann::json;

const std::vector<std::string> kStageNames = {
    "intake", "strategy", "investigation", "synthesis", "boundary",
    "planning", "critique", "revision", "complete"};
const std::vector<std::string> kStateNames = {
    "running", "awaiting_clarification", "awaiting_approval", "approved",
    "failed", "cancelled", "manual_review"};

std::string checkpoint_key(std::string_view tenant_id, std::string_view pipeline_id) {
    return std::string(tenant_id) + "\n" + std::string(pipeline_id);
}

bool checkpoint_valid(const CognitionCheckpoint& value, std::string* error) {
    std::vector<contracts::ContractIssue> issues;
    if(!contracts::validate_metadata(value.metadata, &issues) || value.pipeline_id.empty() ||
       value.intake_digest.empty() || value.revision == 0) {
        if(error) {
            *error = !issues.empty() ? issues.front().message
                                     : "pipeline_id, intake_digest and positive revision are required";
        }
        return false;
    }
    return true;
}

json artifact_json(const CognitionStageArtifact& value) {
    return {{"stage", cognition_stage_name(value.stage)}, {"iteration", value.iteration},
            {"invocation_id", value.invocation_id},
            {"manifest_digest", value.manifest_digest},
            {"output_digest", value.output_digest}, {"provider", value.provider},
            {"model", value.model}, {"independence_group", value.independence_group},
            {"output", value.output}};
}

CognitionStageArtifact artifact_from_json(const json& value) {
    static const std::set<std::string> fields = {
        "stage", "iteration", "invocation_id", "manifest_digest", "output_digest",
        "provider", "model", "independence_group", "output"};
    if(!value.is_object()) throw std::invalid_argument("stage artifact must be an object");
    for(const auto& [key, item] : value.items()) {
        (void)item;
        if(!fields.count(key)) throw std::invalid_argument("unknown stage artifact field: " + key);
    }
    for(const auto& field : fields)
        if(!value.contains(field)) throw std::invalid_argument("missing stage artifact field: " + field);
    const auto stage = cognition_stage_from_name(value.at("stage").get<std::string>());
    if(!stage) throw std::invalid_argument("unknown cognition stage");
    CognitionStageArtifact result;
    result.stage = *stage;
    result.iteration = value.at("iteration").get<std::uint64_t>();
    result.invocation_id = value.at("invocation_id").get<std::string>();
    result.manifest_digest = value.at("manifest_digest").get<std::string>();
    result.output_digest = value.at("output_digest").get<std::string>();
    result.provider = value.at("provider").get<std::string>();
    result.model = value.at("model").get<std::string>();
    result.independence_group = value.at("independence_group").get<std::string>();
    result.output = value.at("output");
    return result;
}

const CognitionStageArtifact* latest_stage(const CognitionCheckpoint& checkpoint,
                                           CognitionStage stage) {
    const CognitionStageArtifact* result = nullptr;
    for(const auto& artifact : checkpoint.artifacts)
        if(artifact.stage == stage && (!result || artifact.iteration >= result->iteration))
            result = &artifact;
    return result;
}

std::string default_now() { return audit_timestamp_now(); }

std::string view_profile(CognitionStage stage) {
    switch(stage) {
        case CognitionStage::Intake: return "intake";
        case CognitionStage::Strategy:
        case CognitionStage::Investigation:
        case CognitionStage::Synthesis: return "investigation";
        case CognitionStage::Revision: return "replan";
        default: return "planning";
    }
}

memory_v2::MemoryViewMode view_mode(CognitionStage stage) {
    switch(stage) {
        case CognitionStage::Intake: return memory_v2::MemoryViewMode::Intake;
        case CognitionStage::Strategy:
        case CognitionStage::Investigation:
        case CognitionStage::Synthesis: return memory_v2::MemoryViewMode::Investigation;
        case CognitionStage::Revision: return memory_v2::MemoryViewMode::Replan;
        default: return memory_v2::MemoryViewMode::Planning;
    }
}

json memory_context(const memory_v2::MemoryView& view) {
    json records = json::array();
    for(const auto& record : view.records) {
        records.push_back({{"record_id", record.record_id}, {"revision", record.revision},
                           {"source_kind", record.source_kind},
                           {"source_locator", record.source_locator},
                           {"source_digest", record.source_digest},
                           {"trust_class", record.trust_class},
                           {"freshness_deadline", record.freshness_deadline},
                           {"content", record.content}});
    }
    return {{"snapshot_id", view.snapshot.snapshot_id},
            {"view_digest", view.manifest.view_digest}, {"records", std::move(records)}};
}

std::vector<std::string> strings(const json& object, const char* field) {
    if(!object.contains(field) || !object.at(field).is_array())
        throw std::invalid_argument(std::string(field) + " must be an array");
    return object.at(field).get<std::vector<std::string>>();
}

bool require_object_fields(const json& value, const std::set<std::string>& required,
                           std::string* error) {
    if(!value.is_object()) {
        if(error) *error = "stage output must be an object";
        return false;
    }
    for(const auto& field : required) {
        if(!value.contains(field)) {
            if(error) *error = "stage output missing field: " + field;
            return false;
        }
    }
    for(const auto& [field, item] : value.items()) {
        (void)item;
        if(!required.count(field)) {
            if(error) *error = "stage output contains unknown field: " + field;
            return false;
        }
    }
    return true;
}

bool intake_valid(const json& output, std::string* error) {
    try {
        if(!require_object_fields(output,
            {"goal", "constraints", "acceptance_criteria", "ambiguities", "risks",
             "clarification_required", "clarification_questions"}, error)) return false;
        if(!output.at("goal").is_string() || output.at("goal").get<std::string>().empty())
            throw std::invalid_argument("intake goal must be a non-empty string");
        (void)strings(output, "constraints");
        (void)strings(output, "acceptance_criteria");
        (void)strings(output, "ambiguities");
        (void)strings(output, "risks");
        (void)strings(output, "clarification_questions");
        if(!output.at("clarification_required").is_boolean())
            throw std::invalid_argument("clarification_required must be boolean");
        return true;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return false;
    }
}

bool strategy_valid(const json& output, std::string* error) {
    try {
        if(!require_object_fields(output,
            {"fact_gaps", "source_priorities", "steps", "max_tool_calls",
             "stop_conditions"}, error)) return false;
        (void)strings(output, "fact_gaps");
        (void)strings(output, "source_priorities");
        (void)strings(output, "stop_conditions");
        if(!output.at("max_tool_calls").is_number_unsigned() &&
           !output.at("max_tool_calls").is_number_integer())
            throw std::invalid_argument("max_tool_calls must be an integer");
        if(output.at("max_tool_calls").get<std::int64_t>() < 0)
            throw std::invalid_argument("max_tool_calls must be non-negative");
        if(!output.at("steps").is_array())
            throw std::invalid_argument("strategy steps must be an array");
        for(const auto& step : output.at("steps")) {
            if(!require_object_fields(step,
                {"investigator_id", "question", "round", "required"}, error)) return false;
            if(!step.at("investigator_id").is_string() ||
               step.at("investigator_id").get<std::string>().empty() ||
               !step.at("question").is_string() || !step.at("round").is_number_integer() ||
               step.at("round").get<std::int64_t>() <= 0 || !step.at("required").is_boolean())
                throw std::invalid_argument("strategy step fields have invalid types or values");
        }
        return true;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return false;
    }
}

bool boundary_valid(const json& output, std::string* error) {
    try {
        if(!require_object_fields(output,
            {"change_mode", "rationale", "in_scope", "out_of_scope", "upstream_contracts",
             "downstream_contracts", "blast_radius", "risks"}, error)) return false;
        if(!output.at("change_mode").is_string() ||
           !change_mode_from_string(output.at("change_mode").get<std::string>()))
            throw std::invalid_argument("invalid change_mode");
        if(!output.at("rationale").is_string() || !output.at("blast_radius").is_string())
            throw std::invalid_argument("boundary rationale and blast_radius must be strings");
        (void)strings(output, "in_scope");
        (void)strings(output, "out_of_scope");
        (void)strings(output, "upstream_contracts");
        (void)strings(output, "downstream_contracts");
        (void)strings(output, "risks");
        return true;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return false;
    }
}

std::optional<TaskUnderstanding> parse_understanding(
    const json& value, const TaskIntake& intake, const json& boundary,
    const std::set<std::string>& valid_evidence, std::string* error) {
    try {
        if(!require_object_fields(value,
            {"domain", "current_state", "target_state", "gaps", "assumptions",
             "unknowns", "risks", "evidence_ids"}, error)) return std::nullopt;
        TaskUnderstanding result;
        result.metadata = intake.metadata;
        result.domain = value.at("domain").get<std::string>();
        result.current_state = value.at("current_state").get<std::string>();
        result.target_state = value.at("target_state").get<std::string>();
        result.gaps = strings(value, "gaps");
        result.assumptions = strings(value, "assumptions");
        result.unknowns = strings(value, "unknowns");
        result.risks = strings(value, "risks");
        result.evidence_ids = strings(value, "evidence_ids");
        for(const auto& evidence_id : result.evidence_ids) {
            if(!valid_evidence.count(evidence_id)) {
                if(error) *error = "understanding references unknown evidence: " + evidence_id;
                return std::nullopt;
            }
        }
        const auto mode = change_mode_from_string(boundary.at("change_mode").get<std::string>());
        if(!mode) {
            if(error) *error = "boundary returned unknown change_mode";
            return std::nullopt;
        }
        result.change_mode = *mode;
        result.blast_radius = boundary.at("blast_radius").get<std::string>();
        if(result.domain.empty() || result.current_state.empty() || result.target_state.empty()) {
            if(error) *error = "understanding domain/current/target must be non-empty";
            return std::nullopt;
        }
        return result;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return std::nullopt;
    }
}

std::optional<ExecutionPlan> parse_plan(
    const json& value, const TaskIntake& intake, const TaskUnderstanding& understanding,
    const EvidenceBundle& evidence, const memory_v2::MemoryView& view,
    std::uint64_t revision, std::string parent_digest,
    const std::set<std::string>& valid_evidence, std::string* error) {
    try {
        if(!require_object_fields(value,
            {"acceptance_contract_digest", "nodes", "critical_path", "budget"}, error))
            return std::nullopt;
        ExecutionPlan result;
        result.metadata = intake.metadata;
        if(result.metadata.identity.plan_id.empty())
            result.metadata.identity.plan_id = intake.metadata.identity.task_id + ":plan";
        result.plan_revision = revision;
        result.parent_plan_digest = std::move(parent_digest);
        result.task_understanding_digest = encode(understanding).at("canonical_digest");
        result.evidence_bundle_digest = encode(evidence).at("canonical_digest");
        result.acceptance_contract_digest =
            value.at("acceptance_contract_digest").get<std::string>();
        result.memory_snapshot_id = view.snapshot.snapshot_id;
        result.planning_view_digest = view.manifest.view_digest;
        if(!value.at("nodes").is_array()) throw std::invalid_argument("nodes must be an array");
        for(const auto& item : value.at("nodes")) {
            if(!require_object_fields(item,
                {"node_id", "objective", "in_scope", "out_of_scope", "input_contracts",
                 "output_contracts", "dependencies", "required_capabilities", "side_effects",
                 "acceptance_contract_id", "rollback_strategy", "risk_level",
                 "approval_required", "basis_refs"}, error)) return std::nullopt;
            PlanNode node;
            node.node_id = item.at("node_id").get<std::string>();
            node.objective = item.at("objective").get<std::string>();
            node.in_scope = strings(item, "in_scope");
            node.out_of_scope = strings(item, "out_of_scope");
            node.input_contracts = strings(item, "input_contracts");
            node.output_contracts = strings(item, "output_contracts");
            node.dependencies = strings(item, "dependencies");
            node.required_capabilities = strings(item, "required_capabilities");
            node.side_effects = strings(item, "side_effects");
            node.acceptance_contract_id = item.at("acceptance_contract_id").get<std::string>();
            node.rollback_strategy = item.at("rollback_strategy").get<std::string>();
            node.risk_level = item.at("risk_level").get<std::string>();
            node.approval_required = item.at("approval_required").get<bool>();
            const auto basis_refs = strings(item, "basis_refs");
            if(basis_refs.empty()) {
                if(error) *error = "plan node has no evidence or assumption basis: " + node.node_id;
                return std::nullopt;
            }
            for(const auto& reference : basis_refs) {
                const bool assumption = reference.rfind("assumption:", 0) == 0 &&
                                        reference.size() > std::string("assumption:").size();
                if(!assumption && !valid_evidence.count(reference)) {
                    if(error) *error = "plan node references unknown basis: " + reference;
                    return std::nullopt;
                }
                node.input_contracts.push_back("basis:" + reference);
            }
            result.nodes.push_back(std::move(node));
        }
        result.critical_path = strings(value, "critical_path");
        const auto& budget = value.at("budget");
        result.budget.wall_time_ms = budget.value("wall_time_ms", 0ULL);
        result.budget.token_budget = budget.value("token_budget", 0ULL);
        result.budget.tool_calls = budget.value("tool_calls", 0ULL);
        result.budget.cost_limit = budget.value("cost_limit", 0.0);
        if(result.acceptance_contract_digest.empty()) {
            if(error) *error = "acceptance_contract_digest is required";
            return std::nullopt;
        }
        return result;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return std::nullopt;
    }
}

bool synthesis_valid(const json& output, const std::set<std::string>& evidence_ids,
                     std::string* error) {
    try {
        if(!require_object_fields(output, {"claims", "conflicts", "unknowns"}, error)) return false;
        if(!output.at("claims").is_array())
            throw std::invalid_argument("claims must be an array");
        if(!output.at("conflicts").is_array())
            throw std::invalid_argument("conflicts must be an array");
        (void)strings(output, "unknowns");
        for(const auto& claim : output.at("claims")) {
            if(!require_object_fields(claim,
                {"claim_id", "statement", "evidence_ids", "assumption", "confidence"}, error))
                return false;
            if(!claim.at("claim_id").is_string() || !claim.at("statement").is_string() ||
               !claim.at("assumption").is_boolean() || !claim.at("confidence").is_number())
                throw std::invalid_argument("claim fields have invalid types");
            const auto confidence = claim.at("confidence").get<double>();
            if(confidence < 0.0 || confidence > 1.0)
                throw std::invalid_argument("claim confidence must be within [0,1]");
            const auto references = strings(claim, "evidence_ids");
            const bool assumption = claim.at("assumption").get<bool>();
            if(references.empty() && !assumption)
                throw std::invalid_argument("claim has neither evidence nor explicit assumption");
            for(const auto& reference : references) {
                if(!evidence_ids.count(reference))
                    throw std::invalid_argument("claim references unknown evidence: " + reference);
            }
        }
        return true;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return false;
    }
}

bool critique_approved(const json& output, const std::set<std::string>& evidence_ids,
                       std::string* error) {
    try {
        if(!require_object_fields(output, {"approved", "findings", "counterexamples"}, error))
            return false;
        if(!output.at("approved").is_boolean() || !output.at("findings").is_array())
            throw std::invalid_argument("critic approved/findings have invalid types");
        (void)strings(output, "counterexamples");
        bool blocking = false;
        for(const auto& finding : output.at("findings")) {
            if(!require_object_fields(finding,
                {"code", "severity", "node_id", "message", "evidence_ids",
                 "counterexample", "remediation"}, error)) return false;
            if(!finding.at("code").is_string() || !finding.at("severity").is_string() ||
               !finding.at("node_id").is_string() || !finding.at("message").is_string() ||
               !finding.at("counterexample").is_string() ||
               !finding.at("remediation").is_string())
                throw std::invalid_argument("critic finding fields have invalid types");
            const auto severity = finding.at("severity").get<std::string>();
            if(severity != "info" && severity != "warning" && severity != "error" &&
               severity != "critical")
                throw std::invalid_argument("critic finding has invalid severity");
            for(const auto& reference : strings(finding, "evidence_ids"))
                if(!evidence_ids.count(reference))
                    throw std::invalid_argument("critic references unknown evidence: " + reference);
            if(severity == "error" || severity == "critical") blocking = true;
        }
        return output.at("approved").get<bool>() && !blocking;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return false;
    }
}

json intake_json(const TaskIntake& intake) {
    return {{"user_goal", intake.user_goal},
            {"requested_deliverables", intake.requested_deliverables},
            {"explicit_constraints", intake.explicit_constraints},
            {"granted_authorities", intake.granted_authorities},
            {"success_signals", intake.success_signals}, {"fact_gaps", intake.fact_gaps}};
}

void fill_result(CognitionPipelineResult& result, const CognitionCheckpoint& checkpoint,
                 EvidenceStore& evidence_store, PlanStore& plan_store) {
    result.state = checkpoint.state;
    result.checkpoint = checkpoint;
    result.error_code = checkpoint.error_code;
    result.error_message = checkpoint.error_message;
    result.evidence = evidence_store.bundle(checkpoint.metadata, checkpoint.evidence_ids);
    result.plan = plan_store.current(checkpoint.metadata.identity);
    const auto* artifact = latest_stage(checkpoint, CognitionStage::Revision);
    if(!artifact) artifact = latest_stage(checkpoint, CognitionStage::Planning);
    if(artifact) {
        const auto& understanding = artifact->output.value("understanding", json::object());
        const auto* boundary_artifact = latest_stage(checkpoint, CognitionStage::Boundary);
        std::set<std::string> valid(checkpoint.evidence_ids.begin(), checkpoint.evidence_ids.end());
        std::string ignored;
        if(boundary_artifact) {
            TaskIntake intake;
            intake.metadata = checkpoint.metadata;
            result.understanding = parse_understanding(
                understanding, intake, boundary_artifact->output, valid, &ignored);
        }
    }
    if(const auto* intake = latest_stage(checkpoint, CognitionStage::Intake))
        result.clarification_questions =
            intake->output.value("clarification_questions", std::vector<std::string>{});
}

}  // namespace

bool StoreBackedPlanApprovalResolver::approved(
    const contracts::ContractIdentity& identity,std::string_view plan_digest,
    std::string_view decision_id,std::string_view now,std::string* error) {
    const auto decision=store_.latest_decision(decision_id);
    const auto request=store_.request(decision_id);
    if(!decision||!request) { if(error)*error="approval request or decision not found";return false; }
    const auto request_digest=approval::encode(*request).at("canonical_digest").get<std::string>();
    const bool identity_ok=request->metadata.identity.tenant_id==identity.tenant_id&&
        request->metadata.identity.task_id==identity.task_id&&
        (identity.run_id.empty()||request->metadata.identity.run_id==identity.run_id);
    const bool valid=identity_ok&&decision->decision==approval::Decision::Approved&&
        decision->request_digest==request_digest&&request->plan_digest==plan_digest&&
        decision->plan_digest==plan_digest&&decision->policy_revision==request->policy_revision&&
        (decision->expires_at.empty()||now<decision->expires_at)&&
        (request->expires_at.empty()||now<request->expires_at);
    if(!valid&&error)*error="approval identity, digest, policy, state or expiry mismatch";
    return valid;
}

std::string cognition_stage_name(CognitionStage value) {
    return kStageNames.at(static_cast<std::size_t>(value));
}

std::optional<CognitionStage> cognition_stage_from_name(std::string_view value) {
    for(std::size_t i = 0; i < kStageNames.size(); ++i)
        if(kStageNames[i] == value) return static_cast<CognitionStage>(i);
    return std::nullopt;
}

std::string cognition_pipeline_state_name(CognitionPipelineState value) {
    return kStateNames.at(static_cast<std::size_t>(value));
}

std::optional<CognitionPipelineState> cognition_pipeline_state_from_name(std::string_view value) {
    for(std::size_t i = 0; i < kStateNames.size(); ++i)
        if(kStateNames[i] == value) return static_cast<CognitionPipelineState>(i);
    return std::nullopt;
}

json encode(const CognitionCheckpoint& value) {
    json artifacts = json::array();
    for(const auto& artifact : value.artifacts) artifacts.push_back(artifact_json(artifact));
    return contracts::make_typed_contract(value.metadata, "agent.cognition_checkpoint/v1",
        {{"pipeline_id", value.pipeline_id}, {"revision", value.revision},
         {"state", cognition_pipeline_state_name(value.state)},
         {"next_stage", cognition_stage_name(value.next_stage)},
         {"critic_iteration", value.critic_iteration},
         {"tool_calls_used", value.tool_calls_used}, {"evidence_added", value.evidence_added},
         {"investigation_stop_reason", value.investigation_stop_reason},
         {"stage_attempts", value.stage_attempts},
         {"completed_stages", value.completed_stages}, {"evidence_ids", value.evidence_ids},
         {"artifacts", std::move(artifacts)},
         {"intake_digest", value.intake_digest},
         {"evidence_bundle_digest", value.evidence_bundle_digest},
         {"understanding_digest", value.understanding_digest},
         {"plan_digest", value.plan_digest}, {"plan_revision", value.plan_revision},
         {"memory_snapshot_id", value.memory_snapshot_id},
         {"memory_view_digest", value.memory_view_digest},
         {"approval_decision_id", value.approval_decision_id},
         {"error_code", value.error_code}, {"error_message", value.error_message},
         {"updated_at", value.updated_at}});
}

std::optional<CognitionCheckpoint> decode_cognition_checkpoint(
    const json& value, const contracts::ParseContext& context,
    std::vector<contracts::ContractIssue>* issues) {
    static const std::set<std::string> fields = {
        "pipeline_id", "revision", "state", "next_stage", "critic_iteration",
        "tool_calls_used", "evidence_added", "investigation_stop_reason", "stage_attempts", "completed_stages", "evidence_ids", "artifacts",
        "intake_digest", "evidence_bundle_digest", "understanding_digest", "plan_digest", "plan_revision",
        "memory_snapshot_id", "memory_view_digest", "approval_decision_id",
        "error_code", "error_message", "updated_at"};
    const auto document = contracts::parse_typed_contract(
        value, "agent.cognition_checkpoint/v1", context, issues);
    if(!document || !contracts::validate_object_fields(document->payload, fields, fields,
        contracts::UnknownFieldPolicy::Reject, nullptr, issues, "/payload")) return std::nullopt;
    try {
        CognitionCheckpoint result;
        result.metadata = document->metadata;
        const auto& payload = document->payload;
        result.pipeline_id = payload.at("pipeline_id").get<std::string>();
        result.revision = payload.at("revision").get<std::uint64_t>();
        const auto state = cognition_pipeline_state_from_name(payload.at("state").get<std::string>());
        const auto stage = cognition_stage_from_name(payload.at("next_stage").get<std::string>());
        if(!state || !stage) throw std::invalid_argument("unknown checkpoint state or stage");
        result.state = *state;
        result.next_stage = *stage;
        result.critic_iteration = payload.at("critic_iteration").get<std::uint64_t>();
        result.tool_calls_used = payload.at("tool_calls_used").get<std::uint64_t>();
        result.evidence_added = payload.at("evidence_added").get<std::uint64_t>();
        result.investigation_stop_reason = payload.at("investigation_stop_reason").get<std::string>();
        result.stage_attempts = payload.at("stage_attempts").get<std::map<std::string,std::uint64_t>>();
        result.completed_stages = payload.at("completed_stages").get<std::vector<std::string>>();
        result.evidence_ids = payload.at("evidence_ids").get<std::vector<std::string>>();
        for(const auto& artifact : payload.at("artifacts"))
            result.artifacts.push_back(artifact_from_json(artifact));
        result.intake_digest = payload.at("intake_digest").get<std::string>();
        result.evidence_bundle_digest = payload.at("evidence_bundle_digest").get<std::string>();
        result.understanding_digest = payload.at("understanding_digest").get<std::string>();
        result.plan_digest = payload.at("plan_digest").get<std::string>();
        result.plan_revision = payload.at("plan_revision").get<std::uint64_t>();
        result.memory_snapshot_id = payload.at("memory_snapshot_id").get<std::string>();
        result.memory_view_digest = payload.at("memory_view_digest").get<std::string>();
        result.approval_decision_id = payload.at("approval_decision_id").get<std::string>();
        result.error_code = payload.at("error_code").get<std::string>();
        result.error_message = payload.at("error_message").get<std::string>();
        result.updated_at = payload.at("updated_at").get<std::string>();
        std::string validation_error;
        if(!checkpoint_valid(result, &validation_error))
            throw std::invalid_argument(validation_error);
        return result;
    } catch(const std::exception& exception) {
        contracts::append_issue(issues, "payload_decode_failed", "/payload", exception.what());
        return std::nullopt;
    }
}

std::string InMemoryCognitionCheckpointStore::key(
    std::string_view tenant_id, std::string_view pipeline_id) {
    return checkpoint_key(tenant_id, pipeline_id);
}

CognitionCheckpointCommit InMemoryCognitionCheckpointStore::create(
    const CognitionCheckpoint& checkpoint) {
    std::string error;
    if(!checkpoint_valid(checkpoint, &error) || checkpoint.revision != 1)
        return {CognitionCheckpointStatus::Invalid, 0, {},
                error.empty() ? "initial checkpoint revision must be 1" : error};
    std::lock_guard lock(mutex_);
    const auto id = key(checkpoint.metadata.identity.tenant_id, checkpoint.pipeline_id);
    if(checkpoints_.count(id)) return {CognitionCheckpointStatus::AlreadyExists, 0, {}, {}};
    const auto digest = encode(checkpoint).at("canonical_digest").get<std::string>();
    checkpoints_.emplace(id, StoredCognitionCheckpoint{checkpoint, 1});
    return {CognitionCheckpointStatus::Committed, 1, digest, {}};
}

std::optional<StoredCognitionCheckpoint> InMemoryCognitionCheckpointStore::load(
    std::string_view tenant_id, std::string_view pipeline_id) {
    std::lock_guard lock(mutex_);
    const auto found = checkpoints_.find(key(tenant_id, pipeline_id));
    return found == checkpoints_.end() ? std::nullopt
                                       : std::optional<StoredCognitionCheckpoint>(found->second);
}

CognitionCheckpointCommit InMemoryCognitionCheckpointStore::compare_exchange(
    const CognitionCheckpoint& checkpoint, std::uint64_t expected_revision) {
    std::string error;
    if(!checkpoint_valid(checkpoint, &error) || checkpoint.revision != expected_revision + 1)
        return {CognitionCheckpointStatus::Invalid, 0, {},
                error.empty() ? "checkpoint revision must advance exactly once" : error};
    std::lock_guard lock(mutex_);
    const auto id = key(checkpoint.metadata.identity.tenant_id, checkpoint.pipeline_id);
    const auto found = checkpoints_.find(id);
    if(found == checkpoints_.end()) return {CognitionCheckpointStatus::NotFound, 0, {}, {}};
    if(found->second.revision != expected_revision)
        return {CognitionCheckpointStatus::RevisionConflict, found->second.revision, {}, {}};
    found->second = {checkpoint, checkpoint.revision};
    const auto digest = encode(checkpoint).at("canonical_digest").get<std::string>();
    return {CognitionCheckpointStatus::Committed, checkpoint.revision, digest, {}};
}

RoleRuntimeCognitionModel::RoleRuntimeCognitionModel(
    std::shared_ptr<llm_runtime::RoleRuntime> runtime) : runtime_(std::move(runtime)) {
    if(!runtime_) throw std::invalid_argument("RoleRuntime is required");
}

bool RoleRuntimeCognitionModel::bind(CognitionStage stage, CognitionRoleBinding binding) {
    if(stage == CognitionStage::Investigation || stage == CognitionStage::Complete ||
       binding.profile_id.empty() || binding.profile_revision.empty()) return false;
    return bindings_.emplace(stage, std::move(binding)).second;
}

CognitionStageResponse RoleRuntimeCognitionModel::invoke(const CognitionStageRequest& request) {
    CognitionStageResponse result;
    if(request.cancelled && request.cancelled()) {
        result.error_code = "cognition_cancelled";
        result.error_message = "cognition stage cancelled before LLM invocation";
        return result;
    }
    const auto binding = bindings_.find(request.stage);
    if(binding == bindings_.end()) {
        result.error_code = "cognition_role_unbound";
        result.error_message = "no RoleRuntime profile is bound to " + cognition_stage_name(request.stage);
        return result;
    }
    const auto input_text = contracts::canonical_json(request.input);
    llm_runtime::RoleInvocationRequest invocation;
    invocation.metadata = request.metadata;
    invocation.invocation_id = request.pipeline_id + ":" + cognition_stage_name(request.stage) +
                               ":" + std::to_string(request.iteration) +
                               ":" + std::to_string(request.attempt);
    invocation.trace_id = request.metadata.identity.run_id.empty()
        ? request.pipeline_id : request.metadata.identity.run_id;
    invocation.profile_id = binding->second.profile_id;
    invocation.profile_revision = binding->second.profile_revision;
    invocation.prompt_variables = {{"input", input_text}};
    invocation.input.context = memory_context(request.memory_view).dump();
    invocation.memory_view = {request.memory_view.snapshot.snapshot_id,
                              view_profile(request.stage),
                              request.memory_view.manifest.view_digest};
    invocation.granted_capabilities = binding->second.granted_capabilities;
    invocation.independence = request.independence;
    invocation.required_region = binding->second.required_region;
    invocation.estimated_input_tokens =
        static_cast<std::uint64_t>((input_text.size() + invocation.input.context.size() + 3) / 4);
    invocation.policy_revision = "phase4-v2-f2c-r1";
    auto runtime_result = runtime_->invoke(std::move(invocation));
    result.manifest = runtime_result.manifest;
    result.error_code = runtime_result.error_code;
    result.error_message = runtime_result.error_message;
    if(!runtime_result.ok || !runtime_result.structured_output) return result;
    if(request.cancelled && request.cancelled()) {
        result.error_code = "cognition_cancelled";
        result.error_message = "cognition stage cancelled after LLM invocation";
        return result;
    }
    result.ok = true;
    result.output = *runtime_result.structured_output;
    return result;
}

MultiStageCognitionWorkflow::MultiStageCognitionWorkflow(
    memory_v2::MemoryViewEngine& views, InvestigatorRegistry& investigators,
    EvidenceStore& evidence, PlanStore& plans, CognitionCheckpointStore& checkpoints,
    CognitionStageModel& model)
    : views_(views), investigators_(investigators), evidence_(evidence), plans_(plans),
      checkpoints_(checkpoints), model_(model) {}

CognitionPipelineResult MultiStageCognitionWorkflow::run(
    const TaskIntake& intake, const memory_v2::MemoryScope& subject,
    const CognitionPipelineOptions& options) {
    CognitionPipelineResult result;
    if(intake.user_goal.empty() || intake.metadata.identity.tenant_id.empty() ||
       intake.metadata.identity.task_id.empty() || intake.metadata.identity.plan_id.empty()) {
        result.error_code = "task_intake_invalid";
        result.error_message = "task intake goal, tenant, task and plan are required";
        return result;
    }
    const auto now = options.now ? options.now : default_now;
    const auto cancelled = [&] { return options.cancelled && options.cancelled(); };
    const std::string pipeline_id = !options.pipeline_id.empty() ? options.pipeline_id
        : !intake.metadata.identity.run_id.empty() ? intake.metadata.identity.run_id + ":cognition"
                                                  : intake.metadata.identity.task_id + ":cognition";
    const auto intake_digest = encode(intake).at("canonical_digest").get<std::string>();
    auto stored = checkpoints_.load(intake.metadata.identity.tenant_id, pipeline_id);
    CognitionCheckpoint checkpoint;
    std::uint64_t store_revision = 0;
    if(stored) {
        checkpoint = stored->checkpoint;
        store_revision = stored->revision;
        if(checkpoint.metadata.identity.task_id != intake.metadata.identity.task_id) {
            result.error_code = "checkpoint_scope_mismatch";
            result.error_message = "cognition checkpoint belongs to another task";
            return result;
        }
        if(checkpoint.intake_digest != intake_digest) {
            result.error_code = "checkpoint_input_mismatch";
            result.error_message = "cognition checkpoint was created for a different task intake";
            return result;
        }
    } else {
        checkpoint.metadata = intake.metadata;
        checkpoint.pipeline_id = pipeline_id;
        checkpoint.intake_digest = intake_digest;
        checkpoint.updated_at = now();
        const auto commit = checkpoints_.create(checkpoint);
        if(!commit) {
            result.error_code = "checkpoint_create_failed";
            result.error_message = commit.error;
            return result;
        }
        store_revision = commit.revision;
    }

    auto emit = [&](std::string event_type, CognitionStage stage, json payload = json::object()) {
        if(options.event_sink)
            options.event_sink({pipeline_id, store_revision, stage,
                                std::move(event_type), std::move(payload)});
    };
    auto persist = [&]() -> bool {
        checkpoint.revision = store_revision + 1;
        checkpoint.updated_at = now();
        const auto commit = checkpoints_.compare_exchange(checkpoint, store_revision);
        if(!commit) {
            checkpoint.state = CognitionPipelineState::ManualReview;
            checkpoint.error_code = commit.status == CognitionCheckpointStatus::RevisionConflict
                ? "checkpoint_revision_conflict" : "checkpoint_write_failed";
            checkpoint.error_message = commit.error;
            return false;
        }
        store_revision = commit.revision;
        return true;
    };
    auto fail = [&](std::string code, std::string message,
                    CognitionPipelineState state = CognitionPipelineState::Failed) {
        checkpoint.state = state;
        checkpoint.error_code = std::move(code);
        checkpoint.error_message = std::move(message);
        persist();
        emit("pipeline_failed", checkpoint.next_stage,
             {{"error_code", checkpoint.error_code}, {"error_message", checkpoint.error_message}});
    };
    auto finish_result = [&]() {
        fill_result(result, checkpoint, evidence_, plans_);
        result.issues = result.plan ? validator_.validate(*result.plan).issues
                                    : std::vector<PlanIssue>{};
        return result;
    };

    if(checkpoint.state == CognitionPipelineState::AwaitingClarification) {
        if(options.clarification_answers.empty()) return finish_result();
        checkpoint.state = CognitionPipelineState::Running;
        checkpoint.next_stage = CognitionStage::Intake;
        checkpoint.error_code.clear();
        checkpoint.error_message.clear();
        if(!persist()) return finish_result();
    } else if(checkpoint.state == CognitionPipelineState::AwaitingApproval) {
        if(!options.plan_revision_request.empty()) {
            checkpoint.state = CognitionPipelineState::Running;
            checkpoint.next_stage = CognitionStage::Revision;
            checkpoint.error_code.clear();
            checkpoint.error_message.clear();
            if(!persist()) return finish_result();
            emit("plan_revision_requested", CognitionStage::Revision,
                 {{"request_digest", contracts::embedded_digest(
                     options.plan_revision_request).value_or("")}});
        } else if(!options.approval_decision_id.empty() &&
                  ((options.approval_resolver&&options.approval_resolver->approved(
                      intake.metadata.identity,checkpoint.plan_digest,
                      options.approval_decision_id,now(),nullptr))||
                   (!options.approval_resolver&&options.approval_validator&&
                    options.approval_validator(checkpoint.plan_digest,
                                               options.approval_decision_id)))) {
            checkpoint.state = CognitionPipelineState::Approved;
            checkpoint.approval_decision_id = options.approval_decision_id;
            if(!persist()) return finish_result();
            emit("plan_approved_by_hitl", CognitionStage::Complete,
                 {{"decision_id", checkpoint.approval_decision_id},
                  {"plan_digest", checkpoint.plan_digest}});
            return finish_result();
        } else {
            return finish_result();
        }
    } else if(checkpoint.state != CognitionPipelineState::Running) {
        return finish_result();
    }

    auto build_view = [&](CognitionStage stage) -> std::optional<memory_v2::MemoryView> {
        auto spec = memory_v2::make_view_spec(view_mode(stage), intake.metadata, subject);
        auto view = views_.build(spec, now());
        if(view.fail_closed) {
            fail("memory_view_failed", cognition_stage_name(stage) + ":" + view.error);
            return std::nullopt;
        }
        checkpoint.memory_snapshot_id = view.snapshot.snapshot_id;
        checkpoint.memory_view_digest = view.manifest.view_digest;
        return view;
    };

    auto invoke_stage = [&](CognitionStage stage, std::uint64_t iteration, const json& input,
                            llm_runtime::IndependenceRequirement independence = {})
        -> const CognitionStageArtifact* {
        for(const auto& artifact : checkpoint.artifacts)
            if(artifact.stage == stage && artifact.iteration == iteration) return &artifact;
        const auto view = build_view(stage);
        if(!view) return nullptr;
        const auto attempt_key = cognition_stage_name(stage) + ":" + std::to_string(iteration);
        while(checkpoint.stage_attempts[attempt_key] < options.max_stage_attempts) {
            if(cancelled()) {
                fail("cognition_cancelled", "cognition cancelled", CognitionPipelineState::Cancelled);
                return nullptr;
            }
            const auto attempt = ++checkpoint.stage_attempts[attempt_key];
            if(!persist()) return nullptr;
            emit("stage_started", stage, {{"iteration", iteration}, {"attempt", attempt}});
            CognitionStageRequest request;
            request.metadata = intake.metadata;
            request.pipeline_id = pipeline_id;
            request.stage = stage;
            request.iteration = iteration;
            request.attempt = attempt;
            request.memory_view = *view;
            request.input = input;
            request.independence = independence;
            request.cancelled = options.cancelled;
            auto response = model_.invoke(request);
            if(response.ok && response.output.is_object()) {
                CognitionStageArtifact artifact;
                artifact.stage = stage;
                artifact.iteration = iteration;
                artifact.invocation_id = response.manifest.invocation_id;
                artifact.manifest_digest = llm_runtime::encode(response.manifest)
                                               .value("canonical_digest", "");
                artifact.output = std::move(response.output);
                artifact.output_digest = contracts::embedded_digest(artifact.output).value_or("");
                artifact.provider = response.manifest.provider;
                artifact.model = response.manifest.model;
                artifact.independence_group = response.manifest.independence_group;
                checkpoint.artifacts.push_back(std::move(artifact));
                checkpoint.completed_stages.push_back(attempt_key);
                checkpoint.error_code.clear();
                checkpoint.error_message.clear();
                if(!persist()) return nullptr;
                emit("stage_completed", stage,
                     {{"iteration", iteration},
                      {"output_digest", checkpoint.artifacts.back().output_digest}});
                return &checkpoint.artifacts.back();
            }
            checkpoint.error_code = response.error_code.empty()
                ? "stage_output_invalid" : response.error_code;
            checkpoint.error_message = response.error_message;
            if(!persist()) return nullptr;
            emit("stage_attempt_failed", stage,
                 {{"iteration", iteration}, {"attempt", attempt},
                  {"error_code", checkpoint.error_code}});
        }
        const auto terminal_code = checkpoint.error_code.empty()
            ? "stage_attempt_budget_exhausted" : checkpoint.error_code;
        const auto terminal_message = cognition_stage_name(stage) +
            " exhausted its stage attempt budget" +
            (checkpoint.error_message.empty() ? std::string()
                                              : ": " + checkpoint.error_message);
        fail(terminal_code, terminal_message);
        return nullptr;
    };

    while(checkpoint.state == CognitionPipelineState::Running) {
        if(cancelled()) {
            fail("cognition_cancelled", "cognition cancelled", CognitionPipelineState::Cancelled);
            break;
        }
        if(checkpoint.next_stage == CognitionStage::Intake) {
            json input = intake_json(intake);
            input["clarification_answers"] = options.clarification_answers;
            const auto intake_iteration = static_cast<std::uint64_t>(std::count_if(
                checkpoint.artifacts.begin(), checkpoint.artifacts.end(), [](const auto& artifact) {
                    return artifact.stage == CognitionStage::Intake;
                }));
            const auto* artifact = invoke_stage(CognitionStage::Intake, intake_iteration, input);
            if(!artifact) break;
            std::string validation_error;
            if(!intake_valid(artifact->output, &validation_error)) {
                fail("intake_output_invalid", validation_error);
                break;
            }
            if(artifact->output.at("clarification_required").get<bool>()) {
                checkpoint.state = CognitionPipelineState::AwaitingClarification;
                checkpoint.next_stage = CognitionStage::Intake;
                persist();
                emit("clarification_required", CognitionStage::Intake,
                     {{"questions", artifact->output.at("clarification_questions")}});
                break;
            }
            checkpoint.next_stage = CognitionStage::Strategy;
            if(!persist()) break;
        } else if(checkpoint.next_stage == CognitionStage::Strategy) {
            const auto* intake_artifact = latest_stage(checkpoint, CognitionStage::Intake);
            const auto* artifact = invoke_stage(CognitionStage::Strategy, 0,
                {{"intake", intake_artifact ? intake_artifact->output : json::object()},
                 {"tool_budget", options.investigator_tool_budget},
                 {"deadline", options.deadline}});
            if(!artifact) break;
            std::string validation_error;
            if(!strategy_valid(artifact->output, &validation_error)) {
                fail("strategy_output_invalid", validation_error);
                break;
            }
            checkpoint.next_stage = CognitionStage::Investigation;
            if(!persist()) break;
        } else if(checkpoint.next_stage == CognitionStage::Investigation) {
            const auto* strategy = latest_stage(checkpoint, CognitionStage::Strategy);
            if(!strategy) { fail("strategy_missing", "investigation requires strategy"); break; }
            std::map<std::string, std::shared_ptr<Investigator>> available;
            for(auto& investigator : investigators_.all()) available[investigator->id()] = investigator;
            const auto declared_budget = strategy->output.value(
                "max_tool_calls", options.investigator_tool_budget);
            const auto budget = std::min<std::uint64_t>(declared_budget,
                                                        options.investigator_tool_budget);
            const auto& steps = strategy->output.at("steps");
            bool stopped = false;
            auto fact_gaps_covered = [&]() {
                if(intake.fact_gaps.empty()) return false;
                std::set<std::string> claims;
                for(const auto& evidence_id : checkpoint.evidence_ids)
                    if(const auto item = evidence_.get(intake.metadata, evidence_id))
                        claims.insert(item->supported_claims.begin(), item->supported_claims.end());
                return std::all_of(intake.fact_gaps.begin(), intake.fact_gaps.end(),
                    [&](const auto& gap) { return claims.count(gap) != 0; });
            };
            for(std::size_t index = 0; index < steps.size(); ++index) {
                const auto step_key = "investigation:" + std::to_string(index);
                if(std::find(checkpoint.completed_stages.begin(), checkpoint.completed_stages.end(),
                             step_key) != checkpoint.completed_stages.end()) continue;
                if(cancelled()) {
                    fail("cognition_cancelled", "investigation cancelled",
                         CognitionPipelineState::Cancelled);
                    stopped = true;
                    break;
                }
                if(checkpoint.tool_calls_used >= budget) {
                    fail("investigation_budget_exhausted", "investigation tool budget exhausted");
                    stopped = true;
                    break;
                }
                const auto& step = steps.at(index);
                if(!step.is_object()) {
                    fail("strategy_step_invalid", "investigation step must be an object");
                    stopped = true;
                    break;
                }
                const auto investigator_id = step.value("investigator_id", "");
                const bool required = step.value("required", true);
                const auto found = available.find(investigator_id);
                if(found == available.end() || !found->second->read_only()) {
                    if(required) {
                        fail(found == available.end() ? "investigator_not_found"
                                                      : "investigator_not_read_only",
                             investigator_id);
                        stopped = true;
                        break;
                    }
                    checkpoint.completed_stages.push_back(step_key);
                    if(!persist()) { stopped = true; break; }
                    continue;
                }
                const auto required_capabilities = found->second->required_capabilities();
                const bool capabilities_granted = std::all_of(
                    required_capabilities.begin(), required_capabilities.end(),
                    [&](const auto& capability) {
                        return std::find(intake.granted_authorities.begin(),
                                         intake.granted_authorities.end(), capability) !=
                               intake.granted_authorities.end();
                    });
                if(!capabilities_granted) {
                    if(required) {
                        fail("investigator_capability_denied", investigator_id);
                        stopped = true;
                        break;
                    }
                    checkpoint.completed_stages.push_back(step_key);
                    if(!persist()) { stopped = true; break; }
                    continue;
                }
                InvestigationRequest request;
                request.intake = intake;
                const auto investigation_view = build_view(CognitionStage::Investigation);
                if(!investigation_view) { stopped = true; break; }
                request.view = *investigation_view;
                request.deadline = options.deadline;
                request.remaining_tool_calls = budget - checkpoint.tool_calls_used;
                request.question = step.value("question", "");
                request.round = step.value("round", 1ULL);
                request.prior_evidence_ids = checkpoint.evidence_ids;
                std::string investigation_error;
                const auto evidence_before = checkpoint.evidence_ids.size();
                auto records = found->second->investigate(request, &investigation_error);
                ++checkpoint.tool_calls_used;
                if(!investigation_error.empty()) {
                    if(required) {
                        fail("investigator_failed", investigator_id + ":" + investigation_error);
                        stopped = true;
                        break;
                    }
                    checkpoint.completed_stages.push_back(step_key);
                    if(!persist()) { stopped = true; break; }
                    continue;
                }
                std::set<std::string> known_digests;
                for(const auto& id : checkpoint.evidence_ids)
                    if(const auto known = evidence_.get(intake.metadata, id))
                        known_digests.insert(known->content_digest);
                for(auto& record : records) {
                    if(record.evidence_id.empty() || record.content_digest.empty()) {
                        fail("investigator_evidence_invalid", investigator_id);
                        stopped = true;
                        break;
                    }
                    if(found->second->external()) record.instruction_authority = false;
                    if(known_digests.count(record.content_digest)) continue;
                    const auto commit = evidence_.append(intake.metadata, record);
                    if(commit.status != PlanningCommitStatus::Committed &&
                       commit.status != PlanningCommitStatus::Duplicate) {
                        fail("evidence_persistence_failed", commit.error);
                        stopped = true;
                        break;
                    }
                    known_digests.insert(record.content_digest);
                    checkpoint.evidence_ids.push_back(record.evidence_id);
                    ++checkpoint.evidence_added;
                }
                if(stopped) break;
                std::sort(checkpoint.evidence_ids.begin(), checkpoint.evidence_ids.end());
                checkpoint.evidence_ids.erase(
                    std::unique(checkpoint.evidence_ids.begin(), checkpoint.evidence_ids.end()),
                    checkpoint.evidence_ids.end());
                checkpoint.completed_stages.push_back(step_key);
                if(!persist()) { stopped = true; break; }
                emit("investigator_completed", CognitionStage::Investigation,
                     {{"investigator_id", investigator_id}, {"step", index},
                      {"evidence_count", checkpoint.evidence_ids.size()}});
                const auto added = checkpoint.evidence_ids.size() - evidence_before;
                if(options.stop_when_fact_gaps_covered && fact_gaps_covered()) {
                    checkpoint.investigation_stop_reason = "fact_gaps_covered";
                    if(!persist()) { stopped = true; break; }
                    emit("investigation_adaptive_stop", CognitionStage::Investigation,
                         {{"reason", checkpoint.investigation_stop_reason}});
                    break;
                }
                if(options.minimum_new_evidence_per_step > 0 &&
                   added < options.minimum_new_evidence_per_step) {
                    checkpoint.investigation_stop_reason = "insufficient_information_gain";
                    if(!persist()) { stopped = true; break; }
                    emit("investigation_adaptive_stop", CognitionStage::Investigation,
                         {{"reason", checkpoint.investigation_stop_reason}});
                    break;
                }
            }
            if(stopped) break;
            if(checkpoint.investigation_stop_reason.empty())
                checkpoint.investigation_stop_reason = checkpoint.tool_calls_used >= budget
                    ? "tool_budget_reached" : "strategy_complete";
            result.evidence = evidence_.bundle(intake.metadata, checkpoint.evidence_ids);
            checkpoint.evidence_bundle_digest =
                encode(result.evidence).at("canonical_digest").get<std::string>();
            checkpoint.completed_stages.push_back("investigation");
            checkpoint.next_stage = CognitionStage::Synthesis;
            if(!persist()) break;
        } else if(checkpoint.next_stage == CognitionStage::Synthesis) {
            result.evidence = evidence_.bundle(intake.metadata, checkpoint.evidence_ids);
            const auto* artifact = invoke_stage(CognitionStage::Synthesis, 0,
                {{"evidence_bundle", encode(result.evidence)},
                 {"strategy", latest_stage(checkpoint, CognitionStage::Strategy)->output}});
            if(!artifact) break;
            const std::set<std::string> evidence_ids(checkpoint.evidence_ids.begin(),
                                                     checkpoint.evidence_ids.end());
            std::string validation_error;
            if(!synthesis_valid(artifact->output, evidence_ids, &validation_error)) {
                fail("synthesis_output_invalid", validation_error);
                break;
            }
            checkpoint.next_stage = CognitionStage::Boundary;
            if(!persist()) break;
        } else if(checkpoint.next_stage == CognitionStage::Boundary) {
            const auto* artifact = invoke_stage(CognitionStage::Boundary, 0,
                {{"intake", latest_stage(checkpoint, CognitionStage::Intake)->output},
                 {"synthesis", latest_stage(checkpoint, CognitionStage::Synthesis)->output}});
            if(!artifact) break;
            std::string validation_error;
            if(!boundary_valid(artifact->output, &validation_error)) {
                fail("boundary_output_invalid", validation_error);
                break;
            }
            checkpoint.next_stage = CognitionStage::Planning;
            if(!persist()) break;
        } else if(checkpoint.next_stage == CognitionStage::Planning ||
                  checkpoint.next_stage == CognitionStage::Revision) {
            const bool revision = checkpoint.next_stage == CognitionStage::Revision;
            const auto stage = revision ? CognitionStage::Revision : CognitionStage::Planning;
            result.evidence = evidence_.bundle(intake.metadata, checkpoint.evidence_ids);
            json input = {{"intake", intake_json(intake)},
                          {"synthesis", latest_stage(checkpoint, CognitionStage::Synthesis)->output},
                          {"boundary", latest_stage(checkpoint, CognitionStage::Boundary)->output},
                          {"evidence_bundle_digest", checkpoint.evidence_bundle_digest}};
            if(revision) {
                const auto current = plans_.current(intake.metadata.identity);
                input["current_plan"] = current ? encode(*current) : json::object();
                input["critic"] = latest_stage(checkpoint, CognitionStage::Critique)->output;
                input["revision_request"] = options.plan_revision_request;
            }
            const auto iteration = revision ? checkpoint.critic_iteration + 1 : 0;
            const auto* artifact = invoke_stage(stage, iteration, input);
            if(!artifact) break;
            std::string parse_error;
            const auto& boundary = latest_stage(checkpoint, CognitionStage::Boundary)->output;
            const std::set<std::string> valid_evidence(checkpoint.evidence_ids.begin(),
                                                       checkpoint.evidence_ids.end());
            const auto understanding = parse_understanding(
                artifact->output.value("understanding", json::object()), intake,
                boundary, valid_evidence, &parse_error);
            if(!understanding) {
                fail("planning_understanding_invalid", parse_error);
                break;
            }
            const auto planning_view = build_view(stage);
            if(!planning_view) break;
            const auto prior = plans_.current(intake.metadata.identity);
            const std::uint64_t plan_revision = revision
                ? (prior ? prior->plan_revision + 1 : 2) : 1;
            const std::string parent_digest = revision && prior
                ? encode(*prior).at("canonical_digest").get<std::string>() : "";
            const auto plan = parse_plan(
                artifact->output.value("plan", json::object()), intake, *understanding,
                result.evidence, *planning_view, plan_revision, parent_digest,
                valid_evidence, &parse_error);
            if(!plan) {
                fail("planning_plan_invalid", parse_error);
                break;
            }
            const auto validation = validator_.validate(*plan);
            result.issues = validation.issues;
            if(!validation.valid()) {
                fail("plan_validation_failed", validation.issues.empty()
                    ? "unknown plan validation error" : validation.issues.front().code);
                break;
            }
            PlanningCommitResult plan_commit;
            if(revision) {
                if(!prior) {
                    fail("plan_revision_missing_parent", "revision requires an existing plan");
                    break;
                }
                plan_commit = plans_.compare_exchange(*plan, prior->plan_revision);
            } else if(prior) {
                const auto prior_digest = encode(*prior).at("canonical_digest").get<std::string>();
                const auto candidate_digest = encode(*plan).at("canonical_digest").get<std::string>();
                if(prior_digest != candidate_digest) {
                    fail("plan_already_exists", "a different plan already exists for this task");
                    break;
                }
                plan_commit = {PlanningCommitStatus::Duplicate, prior_digest, {}};
            } else {
                plan_commit = plans_.create(*plan);
            }
            if(plan_commit.status != PlanningCommitStatus::Committed &&
               plan_commit.status != PlanningCommitStatus::Duplicate) {
                fail("plan_persistence_failed", plan_commit.error);
                break;
            }
            result.understanding = *understanding;
            result.plan = *plan;
            checkpoint.understanding_digest = encode(*understanding).at("canonical_digest");
            checkpoint.plan_digest = encode(*plan).at("canonical_digest");
            checkpoint.plan_revision = plan->plan_revision;
            if(revision) ++checkpoint.critic_iteration;
            checkpoint.next_stage = CognitionStage::Critique;
            if(!persist()) break;
        } else if(checkpoint.next_stage == CognitionStage::Critique) {
            const auto current = plans_.current(intake.metadata.identity);
            if(!current) { fail("plan_missing", "critic requires a persisted plan"); break; }
            const auto planning_artifact = checkpoint.critic_iteration == 0
                ? latest_stage(checkpoint, CognitionStage::Planning)
                : latest_stage(checkpoint, CognitionStage::Revision);
            llm_runtime::IndependenceRequirement independence;
            if(planning_artifact) {
                if(!planning_artifact->independence_group.empty())
                    independence.forbidden_groups.push_back(planning_artifact->independence_group);
                if(!planning_artifact->provider.empty())
                    independence.forbidden_providers.push_back(planning_artifact->provider);
                if(!planning_artifact->model.empty())
                    independence.forbidden_models.push_back(planning_artifact->model);
            }
            const auto* artifact = invoke_stage(CognitionStage::Critique,
                checkpoint.critic_iteration,
                {{"plan", encode(*current)},
                 {"synthesis", latest_stage(checkpoint, CognitionStage::Synthesis)->output},
                 {"boundary", latest_stage(checkpoint, CognitionStage::Boundary)->output}},
                independence);
            if(!artifact) break;
            std::string critique_error;
            const std::set<std::string> critic_evidence(
                checkpoint.evidence_ids.begin(), checkpoint.evidence_ids.end());
            const bool approved = critique_approved(
                artifact->output, critic_evidence, &critique_error);
            if(!critique_error.empty()) {
                fail("critique_output_invalid", critique_error);
                break;
            }
            if(approved) {
                checkpoint.next_stage = CognitionStage::Complete;
                checkpoint.state = std::any_of(current->nodes.begin(), current->nodes.end(),
                    [](const auto& node) { return node.approval_required; })
                    ? CognitionPipelineState::AwaitingApproval
                    : CognitionPipelineState::Approved;
                checkpoint.error_code.clear();
                checkpoint.error_message.clear();
                if(!persist()) break;
                emit(checkpoint.state == CognitionPipelineState::Approved
                         ? "plan_approved" : "approval_required",
                     CognitionStage::Complete,
                     {{"plan_digest", checkpoint.plan_digest},
                      {"plan_revision", checkpoint.plan_revision}});
                break;
            }
            if(checkpoint.critic_iteration >= options.max_critic_revisions) {
                fail("critic_revision_budget_exhausted",
                     "critic did not approve within the revision budget");
                break;
            }
            checkpoint.next_stage = CognitionStage::Revision;
            if(!persist()) break;
        } else {
            fail("cognition_stage_invalid", "unexpected cognition stage");
            break;
        }
    }
    return finish_result();
}

}  // namespace agent_framework::planning
