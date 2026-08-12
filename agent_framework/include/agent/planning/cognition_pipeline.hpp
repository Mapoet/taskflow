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
#include "agent/planning/cognition_workflow.hpp"
#include "agent/approval/store.hpp"

namespace agent_framework::planning {

enum class CognitionStage {
    Intake,
    Strategy,
    Investigation,
    Synthesis,
    Boundary,
    Planning,
    Critique,
    Revision,
    Complete
};

enum class CognitionPipelineState {
    Running,
    AwaitingClarification,
    AwaitingApproval,
    Approved,
    Failed,
    Cancelled,
    ManualReview
};

struct CognitionStageArtifact {
    CognitionStage stage{CognitionStage::Intake};
    std::uint64_t iteration{0};
    std::string invocation_id;
    std::string manifest_digest;
    std::string output_digest;
    std::string provider;
    std::string model;
    std::string independence_group;
    nlohmann::json output = nlohmann::json::object();
};

struct CognitionCheckpoint {
    contracts::ContractMetadata metadata;
    std::string pipeline_id;
    std::uint64_t revision{1};
    CognitionPipelineState state{CognitionPipelineState::Running};
    CognitionStage next_stage{CognitionStage::Intake};
    std::uint64_t critic_iteration{0};
    std::uint64_t tool_calls_used{0};
    std::uint64_t evidence_added{0};
    std::string investigation_stop_reason;
    std::map<std::string, std::uint64_t> stage_attempts;
    std::vector<std::string> completed_stages;
    std::vector<std::string> evidence_ids;
    std::vector<CognitionStageArtifact> artifacts;
    std::string intake_digest;
    std::string evidence_bundle_digest;
    std::string understanding_digest;
    std::string plan_digest;
    std::uint64_t plan_revision{0};
    std::string memory_snapshot_id;
    std::string memory_view_digest;
    std::string approval_decision_id;
    std::string error_code;
    std::string error_message;
    std::string updated_at;
};

std::string cognition_stage_name(CognitionStage value);
std::optional<CognitionStage> cognition_stage_from_name(std::string_view value);
std::string cognition_pipeline_state_name(CognitionPipelineState value);
std::optional<CognitionPipelineState> cognition_pipeline_state_from_name(std::string_view value);
nlohmann::json encode(const CognitionCheckpoint& value);
std::optional<CognitionCheckpoint> decode_cognition_checkpoint(
    const nlohmann::json& value,
    const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);

enum class CognitionCheckpointStatus {
    Committed,
    AlreadyExists,
    NotFound,
    RevisionConflict,
    Invalid,
    Busy,
    Error
};

struct CognitionCheckpointCommit {
    CognitionCheckpointStatus status{CognitionCheckpointStatus::Error};
    std::uint64_t revision{0};
    std::string digest;
    std::string error;
    explicit operator bool() const noexcept {
        return status == CognitionCheckpointStatus::Committed;
    }
};

struct StoredCognitionCheckpoint {
    CognitionCheckpoint checkpoint;
    std::uint64_t revision{0};
};

class CognitionCheckpointStore {
public:
    virtual ~CognitionCheckpointStore() = default;
    virtual CognitionCheckpointCommit create(const CognitionCheckpoint& checkpoint) = 0;
    virtual std::optional<StoredCognitionCheckpoint> load(
        std::string_view tenant_id, std::string_view pipeline_id) = 0;
    virtual CognitionCheckpointCommit compare_exchange(
        const CognitionCheckpoint& checkpoint, std::uint64_t expected_revision) = 0;
};

class InMemoryCognitionCheckpointStore final : public CognitionCheckpointStore {
public:
    CognitionCheckpointCommit create(const CognitionCheckpoint& checkpoint) override;
    std::optional<StoredCognitionCheckpoint> load(
        std::string_view tenant_id, std::string_view pipeline_id) override;
    CognitionCheckpointCommit compare_exchange(
        const CognitionCheckpoint& checkpoint, std::uint64_t expected_revision) override;

private:
    static std::string key(std::string_view tenant_id, std::string_view pipeline_id);
    std::mutex mutex_;
    std::map<std::string, StoredCognitionCheckpoint> checkpoints_;
};

struct SQLiteCognitionCheckpointStoreOptions {
    int busy_timeout_ms{3000};
    bool require_private_permissions{true};
};

class SQLiteCognitionCheckpointStore final : public CognitionCheckpointStore {
public:
    explicit SQLiteCognitionCheckpointStore(
        std::string path, SQLiteCognitionCheckpointStoreOptions options = {});
    ~SQLiteCognitionCheckpointStore() override;
    SQLiteCognitionCheckpointStore(const SQLiteCognitionCheckpointStore&) = delete;
    SQLiteCognitionCheckpointStore& operator=(const SQLiteCognitionCheckpointStore&) = delete;

    CognitionCheckpointCommit create(const CognitionCheckpoint& checkpoint) override;
    std::optional<StoredCognitionCheckpoint> load(
        std::string_view tenant_id, std::string_view pipeline_id) override;
    CognitionCheckpointCommit compare_exchange(
        const CognitionCheckpoint& checkpoint, std::uint64_t expected_revision) override;

    const std::string& path() const noexcept { return path_; }

private:
    void migrate();
    void* db_{nullptr};
    std::string path_;
    SQLiteCognitionCheckpointStoreOptions options_;
    std::mutex mutex_;
};

struct CognitionRoleBinding {
    std::string profile_id;
    std::string profile_revision;
    std::vector<std::string> granted_capabilities;
    std::string required_region;
};

struct CognitionStageRequest {
    contracts::ContractMetadata metadata;
    std::string pipeline_id;
    CognitionStage stage{CognitionStage::Intake};
    std::uint64_t iteration{0};
    std::uint64_t attempt{0};
    memory_v2::MemoryView memory_view;
    nlohmann::json input = nlohmann::json::object();
    llm_runtime::IndependenceRequirement independence;
    std::function<bool()> cancelled;
};

struct CognitionStageResponse {
    bool ok{false};
    nlohmann::json output = nlohmann::json::object();
    llm_runtime::LLMInvocationManifest manifest;
    std::string error_code;
    std::string error_message;
};

class CognitionStageModel {
public:
    virtual ~CognitionStageModel() = default;
    virtual CognitionStageResponse invoke(const CognitionStageRequest& request) = 0;
};

class RoleRuntimeCognitionModel final : public CognitionStageModel {
public:
    explicit RoleRuntimeCognitionModel(std::shared_ptr<llm_runtime::RoleRuntime> runtime);
    bool bind(CognitionStage stage, CognitionRoleBinding binding);
    CognitionStageResponse invoke(const CognitionStageRequest& request) override;

private:
    std::shared_ptr<llm_runtime::RoleRuntime> runtime_;
    std::map<CognitionStage, CognitionRoleBinding> bindings_;
};

struct CognitionPipelineEvent {
    std::string pipeline_id;
    std::uint64_t checkpoint_revision{0};
    CognitionStage stage{CognitionStage::Intake};
    std::string event_type;
    nlohmann::json payload = nlohmann::json::object();
};

struct CognitionPipelineOptions {
    std::string pipeline_id;
    std::string deadline;
    std::uint64_t investigator_tool_budget{64};
    std::uint64_t max_stage_attempts{2};
    std::uint64_t max_critic_revisions{2};
    std::uint64_t minimum_new_evidence_per_step{0};
    bool stop_when_fact_gaps_covered{true};
    nlohmann::json clarification_answers = nlohmann::json::object();
    nlohmann::json plan_revision_request = nlohmann::json::object();
    std::string approval_decision_id;
    std::function<bool(std::string_view plan_digest,
                       std::string_view decision_id)> approval_validator;
    class PlanApprovalResolver* approval_resolver{nullptr};
    std::function<bool()> cancelled;
    std::function<std::string()> now;
    std::function<void(const CognitionPipelineEvent&)> event_sink;
};

class PlanApprovalResolver {
public:
    virtual ~PlanApprovalResolver() = default;
    virtual bool approved(const contracts::ContractIdentity& identity,
                          std::string_view plan_digest,
                          std::string_view decision_id,
                          std::string_view now,
                          std::string* error = nullptr) = 0;
};

class StoreBackedPlanApprovalResolver final : public PlanApprovalResolver {
public:
    explicit StoreBackedPlanApprovalResolver(approval::ApprovalStore& store) : store_(store) {}
    bool approved(const contracts::ContractIdentity&, std::string_view, std::string_view,
                  std::string_view, std::string*) override;
private: approval::ApprovalStore& store_;
};

struct CognitionPipelineResult {
    CognitionPipelineState state{CognitionPipelineState::Failed};
    CognitionCheckpoint checkpoint;
    EvidenceBundle evidence;
    std::optional<TaskUnderstanding> understanding;
    std::optional<ExecutionPlan> plan;
    std::vector<PlanIssue> issues;
    std::vector<std::string> clarification_questions;
    std::string error_code;
    std::string error_message;
};

class MultiStageCognitionWorkflow {
public:
    MultiStageCognitionWorkflow(
        memory_v2::MemoryViewEngine& views,
        InvestigatorRegistry& investigators,
        EvidenceStore& evidence,
        PlanStore& plans,
        CognitionCheckpointStore& checkpoints,
        CognitionStageModel& model);

    CognitionPipelineResult run(
        const TaskIntake& intake,
        const memory_v2::MemoryScope& subject,
        const CognitionPipelineOptions& options = {});

private:
    memory_v2::MemoryViewEngine& views_;
    InvestigatorRegistry& investigators_;
    EvidenceStore& evidence_;
    PlanStore& plans_;
    CognitionCheckpointStore& checkpoints_;
    CognitionStageModel& model_;
    PlanValidator validator_;
};

}  // namespace agent_framework::planning
