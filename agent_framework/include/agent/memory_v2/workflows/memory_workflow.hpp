#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/llm_runtime/runtime.hpp"
#include "agent/memory_v2/governance.hpp"
#include "agent/memory_v2/view_engine.hpp"
#include "agent/memory_v2/view_profiles.hpp"

namespace agent_framework::memory_v2::workflows {

enum class MemoryWorkflowStage {
    Extraction,
    Normalization,
    Consolidation,
    ConflictResolution,
    TaskStateUpdate,
    QueryPlanning,
    Reranking,
    DynamicView,
    GovernanceRecommendation,
    Complete
};

enum class MemoryWorkflowState {
    Running,
    AwaitingClarification,
    AwaitingApproval,
    Completed,
    Failed,
    Cancelled,
    ManualReview
};

struct MemorySourceArtifact {
    std::string artifact_id;
    std::string source_kind;
    std::string source_locator;
    std::string source_digest;
    MemoryScope scope;
    std::vector<std::string> acl_principals;
    nlohmann::json content = nlohmann::json::object();
    std::vector<std::string> evidence_ids;
    std::string event_time;
    bool trusted_instruction{false};
};

struct MemoryWorkflowInput {
    contracts::ContractMetadata metadata;
    std::string workflow_id;
    MemoryScope subject;
    std::string workflow_phase;
    std::string workflow_event;
    std::string risk_level;
    std::string query;
    std::vector<MemorySourceArtifact> sources;
};

struct MemoryStageArtifact {
    MemoryWorkflowStage stage{MemoryWorkflowStage::Extraction};
    std::uint64_t attempt{0};
    std::string invocation_id;
    std::string manifest_digest;
    std::string output_digest;
    std::string provider;
    std::string model;
    std::string independence_group;
    nlohmann::json output = nlohmann::json::object();
};

struct MemoryWorkflowCheckpoint {
    contracts::ContractMetadata metadata;
    std::string workflow_id;
    std::uint64_t revision{1};
    MemoryWorkflowState state{MemoryWorkflowState::Running};
    MemoryWorkflowStage next_stage{MemoryWorkflowStage::Extraction};
    std::map<std::string, std::uint64_t> stage_attempts;
    std::vector<std::string> completed_stages;
    std::vector<MemoryStageArtifact> artifacts;
    std::string input_digest;
    std::string base_snapshot_id;
    std::string base_view_digest;
    nlohmann::json base_view_document = nlohmann::json::object();
    std::string dynamic_snapshot_id;
    std::string dynamic_view_digest;
    nlohmann::json dynamic_view_document = nlohmann::json::object();
    std::vector<std::string> candidate_record_ids;
    std::vector<std::string> candidate_record_digests;
    std::vector<std::string> reranked_record_ids;
    nlohmann::json recommendations = nlohmann::json::array();
    std::string approval_decision_id;
    std::string error_code;
    std::string error_message;
    std::string updated_at;
};

std::string memory_workflow_stage_name(MemoryWorkflowStage value);
std::optional<MemoryWorkflowStage> memory_workflow_stage_from_name(std::string_view value);
std::string memory_workflow_state_name(MemoryWorkflowState value);
std::optional<MemoryWorkflowState> memory_workflow_state_from_name(std::string_view value);
nlohmann::json encode(const MemoryWorkflowCheckpoint& value);
std::optional<MemoryWorkflowCheckpoint> decode_memory_workflow_checkpoint(
    const nlohmann::json& value,
    const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);

enum class MemoryWorkflowStoreStatus {
    Committed,
    AlreadyExists,
    NotFound,
    RevisionConflict,
    Invalid,
    Busy,
    Error
};

struct MemoryWorkflowStoreCommit {
    MemoryWorkflowStoreStatus status{MemoryWorkflowStoreStatus::Error};
    std::uint64_t revision{0};
    std::string digest;
    std::string error;
    explicit operator bool() const noexcept {
        return status == MemoryWorkflowStoreStatus::Committed;
    }
};

struct StoredMemoryWorkflowCheckpoint {
    MemoryWorkflowCheckpoint checkpoint;
    std::uint64_t revision{0};
};

class MemoryWorkflowCheckpointStore {
public:
    virtual ~MemoryWorkflowCheckpointStore() = default;
    virtual MemoryWorkflowStoreCommit create(const MemoryWorkflowCheckpoint& checkpoint) = 0;
    virtual std::optional<StoredMemoryWorkflowCheckpoint> load(
        std::string_view tenant_id, std::string_view workflow_id) = 0;
    virtual MemoryWorkflowStoreCommit compare_exchange(
        const MemoryWorkflowCheckpoint& checkpoint, std::uint64_t expected_revision) = 0;
};

class InMemoryMemoryWorkflowCheckpointStore final : public MemoryWorkflowCheckpointStore {
public:
    MemoryWorkflowStoreCommit create(const MemoryWorkflowCheckpoint& checkpoint) override;
    std::optional<StoredMemoryWorkflowCheckpoint> load(
        std::string_view tenant_id, std::string_view workflow_id) override;
    MemoryWorkflowStoreCommit compare_exchange(
        const MemoryWorkflowCheckpoint& checkpoint, std::uint64_t expected_revision) override;

private:
    static std::string key(std::string_view tenant_id, std::string_view workflow_id);
    std::mutex mutex_;
    std::map<std::string, StoredMemoryWorkflowCheckpoint> checkpoints_;
};

struct SQLiteMemoryWorkflowCheckpointStoreOptions {
    int busy_timeout_ms{3000};
    bool require_private_permissions{true};
};

class SQLiteMemoryWorkflowCheckpointStore final : public MemoryWorkflowCheckpointStore {
public:
    explicit SQLiteMemoryWorkflowCheckpointStore(
        std::string path, SQLiteMemoryWorkflowCheckpointStoreOptions options = {});
    ~SQLiteMemoryWorkflowCheckpointStore() override;
    SQLiteMemoryWorkflowCheckpointStore(const SQLiteMemoryWorkflowCheckpointStore&) = delete;
    SQLiteMemoryWorkflowCheckpointStore& operator=(const SQLiteMemoryWorkflowCheckpointStore&) = delete;

    MemoryWorkflowStoreCommit create(const MemoryWorkflowCheckpoint& checkpoint) override;
    std::optional<StoredMemoryWorkflowCheckpoint> load(
        std::string_view tenant_id, std::string_view workflow_id) override;
    MemoryWorkflowStoreCommit compare_exchange(
        const MemoryWorkflowCheckpoint& checkpoint, std::uint64_t expected_revision) override;

private:
    void migrate();
    void* db_{nullptr};
    std::string path_;
    SQLiteMemoryWorkflowCheckpointStoreOptions options_;
    std::mutex mutex_;
};

struct MemoryRoleBinding {
    std::string profile_id;
    std::string profile_revision;
    std::vector<std::string> granted_capabilities;
    std::string required_region;
};

struct MemoryStageRequest {
    contracts::ContractMetadata metadata;
    std::string workflow_id;
    MemoryWorkflowStage stage{MemoryWorkflowStage::Extraction};
    std::uint64_t attempt{0};
    MemoryView memory_view;
    nlohmann::json input = nlohmann::json::object();
    llm_runtime::IndependenceRequirement independence;
    std::function<bool()> cancelled;
};

struct MemoryStageResponse {
    bool ok{false};
    nlohmann::json output = nlohmann::json::object();
    llm_runtime::LLMInvocationManifest manifest;
    std::string error_code;
    std::string error_message;
};

class MemoryStageModel {
public:
    virtual ~MemoryStageModel() = default;
    virtual MemoryStageResponse invoke(const MemoryStageRequest& request) = 0;
};

class RoleRuntimeMemoryModel final : public MemoryStageModel {
public:
    explicit RoleRuntimeMemoryModel(std::shared_ptr<llm_runtime::RoleRuntime> runtime);
    bool bind(MemoryWorkflowStage stage, MemoryRoleBinding binding);
    MemoryStageResponse invoke(const MemoryStageRequest& request) override;

private:
    std::shared_ptr<llm_runtime::RoleRuntime> runtime_;
    std::map<MemoryWorkflowStage, MemoryRoleBinding> bindings_;
};

struct MemoryWorkflowEvent {
    std::string workflow_id;
    std::uint64_t checkpoint_revision{0};
    MemoryWorkflowStage stage{MemoryWorkflowStage::Extraction};
    std::string event_type;
    nlohmann::json payload = nlohmann::json::object();
};

struct MemoryWorkflowOptions {
    std::uint64_t max_stage_attempts{2};
    ViewBudget maximum_view_budget{};
    std::string approval_decision_id;
    nlohmann::json clarification_answers = nlohmann::json::object();
    std::function<bool(const nlohmann::json&, std::string_view)> governance_validator;
    std::function<bool()> cancelled;
    std::function<std::string()> now;
    std::function<void(const MemoryWorkflowEvent&)> event_sink;
};

struct MemoryWorkflowResult {
    MemoryWorkflowState state{MemoryWorkflowState::Failed};
    MemoryWorkflowCheckpoint checkpoint;
    std::vector<MemoryRecord> candidates;
    MemoryView base_view;
    MemoryView dynamic_view;
    nlohmann::json recommendations = nlohmann::json::array();
    std::string error_code;
    std::string error_message;
};

class MemoryRecommendationExecutor {
public:
    MemoryRecommendationExecutor(std::shared_ptr<MemoryStore> store,
                                 MemoryGovernanceService& governance);
    GovernanceResult execute(
        const nlohmann::json& recommendation,
        std::string_view decision_id,
        std::string_view approval_id,
        const std::function<bool(const nlohmann::json&, std::string_view)>& validator);

private:
    std::shared_ptr<MemoryStore> store_;
    MemoryGovernanceService& governance_;
};

class MultiLayerMemoryWorkflow {
public:
    MultiLayerMemoryWorkflow(
        MemoryViewEngine& views,
        std::shared_ptr<MemoryStore> store,
        MemoryWorkflowCheckpointStore& checkpoints,
        MemoryStageModel& model);

    MemoryWorkflowResult run(
        const MemoryWorkflowInput& input,
        const MemoryWorkflowOptions& options = {});

private:
    MemoryViewEngine& views_;
    std::shared_ptr<MemoryStore> store_;
    MemoryWorkflowCheckpointStore& checkpoints_;
    MemoryStageModel& model_;
};

}  // namespace agent_framework::memory_v2::workflows
