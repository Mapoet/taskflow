#include "agent/memory_v2/workflows/memory_workflow.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

#include "agent/observability/audit.hpp"

namespace agent_framework::memory_v2::workflows {
namespace {

using json = nlohmann::json;

const std::vector<std::string> kStageNames = {
    "extraction", "normalization", "consolidation", "conflict_resolution",
    "task_state_update", "query_planning", "reranking", "dynamic_view",
    "governance_recommendation", "complete"};
const std::vector<std::string> kStateNames = {
    "running", "awaiting_clarification", "awaiting_approval", "completed",
    "failed", "cancelled", "manual_review"};

std::string checkpoint_key(std::string_view tenant_id, std::string_view workflow_id) {
    return std::string(tenant_id) + "\n" + std::string(workflow_id);
}

bool checkpoint_valid(const MemoryWorkflowCheckpoint& value, std::string* error) {
    std::vector<contracts::ContractIssue> issues;
    if(!contracts::validate_metadata(value.metadata, &issues) || value.workflow_id.empty() ||
       value.input_digest.empty() || value.revision == 0) {
        if(error) {
            *error = !issues.empty() ? issues.front().message
                                     : "workflow_id, input_digest and positive revision are required";
        }
        return false;
    }
    return true;
}

json artifact_json(const MemoryStageArtifact& value) {
    return {{"stage", memory_workflow_stage_name(value.stage)}, {"attempt", value.attempt},
            {"invocation_id", value.invocation_id},
            {"manifest_digest", value.manifest_digest},
            {"output_digest", value.output_digest}, {"provider", value.provider},
            {"model", value.model}, {"independence_group", value.independence_group},
            {"output", value.output}};
}

MemoryStageArtifact artifact_from_json(const json& value) {
    static const std::set<std::string> fields = {
        "stage", "attempt", "invocation_id", "manifest_digest", "output_digest",
        "provider", "model", "independence_group", "output"};
    if(!value.is_object()) throw std::invalid_argument("memory stage artifact must be an object");
    for(const auto& [key, item] : value.items()) {
        (void)item;
        if(!fields.count(key)) throw std::invalid_argument("unknown memory stage artifact field: " + key);
    }
    for(const auto& field : fields)
        if(!value.contains(field)) throw std::invalid_argument("missing memory stage artifact field: " + field);
    const auto stage = memory_workflow_stage_from_name(value.at("stage").get<std::string>());
    if(!stage) throw std::invalid_argument("unknown memory workflow stage");
    MemoryStageArtifact result;
    result.stage = *stage;
    result.attempt = value.at("attempt").get<std::uint64_t>();
    result.invocation_id = value.at("invocation_id").get<std::string>();
    result.manifest_digest = value.at("manifest_digest").get<std::string>();
    result.output_digest = value.at("output_digest").get<std::string>();
    result.provider = value.at("provider").get<std::string>();
    result.model = value.at("model").get<std::string>();
    result.independence_group = value.at("independence_group").get<std::string>();
    result.output = value.at("output");
    return result;
}

json memory_context(const MemoryView& view) {
    json records = json::array();
    for(const auto& record : view.records) {
        records.push_back({{"record_id", record.record_id}, {"revision", record.revision},
                           {"level", memory_v2::encode(record).at("payload").at("scope").at("level")},
                           {"kind", memory_v2::encode(record).at("payload").at("kind")},
                           {"authority", memory_v2::encode(record).at("payload").at("authority")},
                           {"status", memory_v2::encode(record).at("payload").at("status")},
                           {"source_kind", record.source_kind},
                           {"source_locator", record.source_locator},
                           {"source_digest", record.source_digest},
                           {"freshness_deadline", record.freshness_deadline},
                           {"content", record.content}});
    }
    return {{"snapshot_id", view.snapshot.snapshot_id},
            {"view_digest", view.manifest.view_digest}, {"records", std::move(records)}};
}

std::string view_profile(MemoryWorkflowStage stage) {
    switch(stage) {
        case MemoryWorkflowStage::Extraction:
        case MemoryWorkflowStage::Normalization:
        case MemoryWorkflowStage::Consolidation:
        case MemoryWorkflowStage::ConflictResolution:
        case MemoryWorkflowStage::TaskStateUpdate: return "memory-consolidation";
        case MemoryWorkflowStage::QueryPlanning:
        case MemoryWorkflowStage::Reranking: return "memory-retrieval";
        case MemoryWorkflowStage::DynamicView: return "memory-view-policy";
        case MemoryWorkflowStage::GovernanceRecommendation: return "memory-governance";
        case MemoryWorkflowStage::Complete: return "memory-complete";
    }
    return "memory";
}

}  // namespace

std::string memory_workflow_stage_name(MemoryWorkflowStage value) {
    return kStageNames.at(static_cast<std::size_t>(value));
}

std::optional<MemoryWorkflowStage> memory_workflow_stage_from_name(std::string_view value) {
    for(std::size_t index = 0; index < kStageNames.size(); ++index)
        if(kStageNames[index] == value) return static_cast<MemoryWorkflowStage>(index);
    return std::nullopt;
}

std::string memory_workflow_state_name(MemoryWorkflowState value) {
    return kStateNames.at(static_cast<std::size_t>(value));
}

std::optional<MemoryWorkflowState> memory_workflow_state_from_name(std::string_view value) {
    for(std::size_t index = 0; index < kStateNames.size(); ++index)
        if(kStateNames[index] == value) return static_cast<MemoryWorkflowState>(index);
    return std::nullopt;
}

json encode(const MemoryWorkflowCheckpoint& value) {
    json artifacts = json::array();
    for(const auto& artifact : value.artifacts) artifacts.push_back(artifact_json(artifact));
    return contracts::make_typed_contract(value.metadata, "agent.memory_workflow_checkpoint/v1",
        {{"workflow_id", value.workflow_id}, {"revision", value.revision},
         {"state", memory_workflow_state_name(value.state)},
         {"next_stage", memory_workflow_stage_name(value.next_stage)},
         {"stage_attempts", value.stage_attempts}, {"completed_stages", value.completed_stages},
         {"artifacts", std::move(artifacts)}, {"input_digest", value.input_digest},
         {"base_snapshot_id", value.base_snapshot_id}, {"base_view_digest", value.base_view_digest},
         {"base_view_document", value.base_view_document},
         {"dynamic_snapshot_id", value.dynamic_snapshot_id},
         {"dynamic_view_digest", value.dynamic_view_digest},
         {"dynamic_view_document", value.dynamic_view_document},
         {"candidate_record_ids", value.candidate_record_ids},
         {"candidate_record_digests", value.candidate_record_digests},
         {"reranked_record_ids", value.reranked_record_ids},
         {"recommendations", value.recommendations},
         {"approval_decision_id", value.approval_decision_id},
         {"error_code", value.error_code}, {"error_message", value.error_message},
         {"updated_at", value.updated_at}});
}

std::optional<MemoryWorkflowCheckpoint> decode_memory_workflow_checkpoint(
    const json& value, const contracts::ParseContext& context,
    std::vector<contracts::ContractIssue>* issues) {
    static const std::set<std::string> fields = {
        "workflow_id", "revision", "state", "next_stage", "stage_attempts",
        "completed_stages", "artifacts", "input_digest", "base_snapshot_id",
        "base_view_digest", "base_view_document", "dynamic_snapshot_id", "dynamic_view_digest",
        "dynamic_view_document",
        "candidate_record_ids", "candidate_record_digests", "reranked_record_ids",
        "recommendations", "approval_decision_id", "error_code", "error_message", "updated_at"};
    auto document = contracts::parse_typed_contract(
        value, "agent.memory_workflow_checkpoint/v1", context, issues, true);
    if(!document || !contracts::validate_object_fields(document->payload, fields, fields,
        contracts::UnknownFieldPolicy::Reject, nullptr, issues, "/payload")) return std::nullopt;
    try {
        MemoryWorkflowCheckpoint result;
        result.metadata = document->metadata;
        const auto& payload = document->payload;
        result.workflow_id = payload.at("workflow_id").get<std::string>();
        result.revision = payload.at("revision").get<std::uint64_t>();
        const auto state = memory_workflow_state_from_name(payload.at("state").get<std::string>());
        const auto stage = memory_workflow_stage_from_name(payload.at("next_stage").get<std::string>());
        if(!state || !stage) throw std::invalid_argument("unknown memory workflow state or stage");
        result.state = *state;
        result.next_stage = *stage;
        result.stage_attempts = payload.at("stage_attempts").get<std::map<std::string, std::uint64_t>>();
        result.completed_stages = payload.at("completed_stages").get<std::vector<std::string>>();
        if(!payload.at("artifacts").is_array()) throw std::invalid_argument("artifacts must be an array");
        for(const auto& artifact : payload.at("artifacts"))
            result.artifacts.push_back(artifact_from_json(artifact));
        result.input_digest = payload.at("input_digest").get<std::string>();
        result.base_snapshot_id = payload.at("base_snapshot_id").get<std::string>();
        result.base_view_digest = payload.at("base_view_digest").get<std::string>();
        result.base_view_document = payload.at("base_view_document");
        result.dynamic_snapshot_id = payload.at("dynamic_snapshot_id").get<std::string>();
        result.dynamic_view_digest = payload.at("dynamic_view_digest").get<std::string>();
        result.dynamic_view_document = payload.at("dynamic_view_document");
        result.candidate_record_ids = payload.at("candidate_record_ids").get<std::vector<std::string>>();
        result.candidate_record_digests = payload.at("candidate_record_digests").get<std::vector<std::string>>();
        result.reranked_record_ids = payload.at("reranked_record_ids").get<std::vector<std::string>>();
        result.recommendations = payload.at("recommendations");
        if(!result.recommendations.is_array()) throw std::invalid_argument("recommendations must be an array");
        result.approval_decision_id = payload.at("approval_decision_id").get<std::string>();
        result.error_code = payload.at("error_code").get<std::string>();
        result.error_message = payload.at("error_message").get<std::string>();
        result.updated_at = payload.at("updated_at").get<std::string>();
        std::string error;
        if(!checkpoint_valid(result, &error)) throw std::invalid_argument(error);
        return result;
    } catch(const std::exception& exception) {
        contracts::append_issue(issues, "payload_decode_failed", "/payload", exception.what());
        return std::nullopt;
    }
}

std::string InMemoryMemoryWorkflowCheckpointStore::key(
    std::string_view tenant_id, std::string_view workflow_id) {
    return checkpoint_key(tenant_id, workflow_id);
}

MemoryWorkflowStoreCommit InMemoryMemoryWorkflowCheckpointStore::create(
    const MemoryWorkflowCheckpoint& checkpoint) {
    std::string error;
    if(!checkpoint_valid(checkpoint, &error) || checkpoint.revision != 1)
        return {MemoryWorkflowStoreStatus::Invalid, 0, {},
                error.empty() ? "initial checkpoint revision must be 1" : error};
    std::lock_guard lock(mutex_);
    const auto id = key(checkpoint.metadata.identity.tenant_id, checkpoint.workflow_id);
    if(checkpoints_.count(id)) return {MemoryWorkflowStoreStatus::AlreadyExists, 0, {}, {}};
    const auto digest = encode(checkpoint).at("canonical_digest").get<std::string>();
    checkpoints_.emplace(id, StoredMemoryWorkflowCheckpoint{checkpoint, 1});
    return {MemoryWorkflowStoreStatus::Committed, 1, digest, {}};
}

std::optional<StoredMemoryWorkflowCheckpoint> InMemoryMemoryWorkflowCheckpointStore::load(
    std::string_view tenant_id, std::string_view workflow_id) {
    std::lock_guard lock(mutex_);
    const auto found = checkpoints_.find(key(tenant_id, workflow_id));
    return found == checkpoints_.end() ? std::nullopt
                                       : std::optional<StoredMemoryWorkflowCheckpoint>(found->second);
}

MemoryWorkflowStoreCommit InMemoryMemoryWorkflowCheckpointStore::compare_exchange(
    const MemoryWorkflowCheckpoint& checkpoint, std::uint64_t expected_revision) {
    std::string error;
    if(!checkpoint_valid(checkpoint, &error) || checkpoint.revision != expected_revision + 1)
        return {MemoryWorkflowStoreStatus::Invalid, 0, {},
                error.empty() ? "checkpoint revision must advance exactly once" : error};
    std::lock_guard lock(mutex_);
    const auto id = key(checkpoint.metadata.identity.tenant_id, checkpoint.workflow_id);
    const auto found = checkpoints_.find(id);
    if(found == checkpoints_.end()) return {MemoryWorkflowStoreStatus::NotFound, 0, {}, {}};
    if(found->second.revision != expected_revision)
        return {MemoryWorkflowStoreStatus::RevisionConflict, found->second.revision, {}, {}};
    found->second = {checkpoint, checkpoint.revision};
    const auto digest = encode(checkpoint).at("canonical_digest").get<std::string>();
    return {MemoryWorkflowStoreStatus::Committed, checkpoint.revision, digest, {}};
}

RoleRuntimeMemoryModel::RoleRuntimeMemoryModel(std::shared_ptr<llm_runtime::RoleRuntime> runtime)
    : runtime_(std::move(runtime)) {
    if(!runtime_) throw std::invalid_argument("RoleRuntime is required");
}

bool RoleRuntimeMemoryModel::bind(MemoryWorkflowStage stage, MemoryRoleBinding binding) {
    if(stage == MemoryWorkflowStage::Complete || binding.profile_id.empty() ||
       binding.profile_revision.empty()) return false;
    return bindings_.emplace(stage, std::move(binding)).second;
}

MemoryStageResponse RoleRuntimeMemoryModel::invoke(const MemoryStageRequest& request) {
    MemoryStageResponse result;
    if(request.cancelled && request.cancelled()) {
        result.error_code = "memory_workflow_cancelled";
        result.error_message = "memory stage cancelled before LLM invocation";
        return result;
    }
    const auto binding = bindings_.find(request.stage);
    if(binding == bindings_.end()) {
        result.error_code = "memory_role_unbound";
        result.error_message = "no RoleRuntime profile is bound to " +
                               memory_workflow_stage_name(request.stage);
        return result;
    }
    const auto input_text = contracts::canonical_json(request.input);
    llm_runtime::RoleInvocationRequest invocation;
    invocation.metadata = request.metadata;
    invocation.invocation_id = request.workflow_id + ":" +
        memory_workflow_stage_name(request.stage) + ":" + std::to_string(request.attempt);
    invocation.trace_id = request.metadata.identity.run_id.empty()
        ? request.workflow_id : request.metadata.identity.run_id;
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
    invocation.estimated_input_tokens = static_cast<std::uint64_t>(
        (input_text.size() + invocation.input.context.size() + 3) / 4);
    invocation.policy_revision = "phase4-v2-f3m-r1";
    auto runtime_result = runtime_->invoke(std::move(invocation));
    result.manifest = runtime_result.manifest;
    result.error_code = runtime_result.error_code;
    result.error_message = runtime_result.error_message;
    if(!runtime_result.ok || !runtime_result.structured_output) return result;
    if(request.cancelled && request.cancelled()) {
        result.error_code = "memory_workflow_cancelled";
        result.error_message = "memory stage cancelled after LLM invocation";
        return result;
    }
    result.ok = true;
    result.output = *runtime_result.structured_output;
    return result;
}

MemoryRecommendationExecutor::MemoryRecommendationExecutor(
    std::shared_ptr<MemoryStore> store, MemoryGovernanceService& governance)
    : store_(std::move(store)), governance_(governance) {
    if(!store_) throw std::invalid_argument("memory store is required");
}

GovernanceResult MemoryRecommendationExecutor::execute(
    const json& recommendation, std::string_view decision_id, std::string_view approval_id,
    const std::function<bool(const json&, std::string_view)>& validator) {
    GovernanceResult result;
    if(!recommendation.is_object() || !validator || decision_id.empty() ||
       !validator(recommendation, decision_id)) {
        result.commit = {CommitStatus::Forbidden, 0, "governance recommendation is not approved"};
        return result;
    }
    const auto action = recommendation.value("action", "");
    const auto record_id = recommendation.value("record_id", "");
    const auto expected = recommendation.value("expected_revision", 0ULL);
    if(record_id.empty() || expected == 0) {
        result.commit = {CommitStatus::Invalid, expected, "record_id and expected_revision are required"};
        return result;
    }
    if(action == "forget") return governance_.forget(record_id, expected, approval_id);
    const auto current = store_->current(record_id);
    if(!current) {
        result.commit = {CommitStatus::NotFound, 0, "record not found"};
        return result;
    }
    const auto current_level = memory_v2::encode(*current).at("payload").at("scope").at("level")
        .get<std::string>();
    if(recommendation.value("target_level", current_level) != current_level) {
        result.commit = {CommitStatus::Forbidden, expected,
                         "cross-scope promotion requires a separately approved copy/migration"};
        return result;
    }
    if(action == "promote_verified") {
        result.commit = governance_.promote(record_id, expected, MemoryStatus::Verified,
                                            Authority::Verified, decision_id, approval_id);
        return result;
    }
    if(action == "promote_authoritative") {
        result.commit = governance_.promote(record_id, expected, MemoryStatus::Authoritative,
                                            Authority::Authoritative, decision_id, approval_id);
        return result;
    }
    result.commit = {CommitStatus::Invalid, expected, "unsupported governance action"};
    return result;
}

}  // namespace agent_framework::memory_v2::workflows
