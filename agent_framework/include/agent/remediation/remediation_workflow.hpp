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

#include "agent/approval/policy.hpp"
#include "agent/assurance/professional_workflow.hpp"
#include "agent/llm_runtime/runtime.hpp"
#include "agent/memory_v2/view_engine.hpp"
#include "agent/memory_v2/view_profiles.hpp"
#include "agent/planning/plan_store.hpp"
#include "agent/planning/plan_validator.hpp"

namespace agent_framework::remediation {

enum class RemediationStage {
    ImpactAnalysis,
    RemediationPlanning,
    PolicyGate,
    PlanCommit,
    ReverificationPlanning,
    Complete
};

enum class RemediationState {
    Running,
    AwaitingApproval,
    ReadyForExecution,
    ManualReview,
    Failed,
    Cancelled
};

struct RequirementBinding {
    std::string requirement_id;
    std::vector<std::string> criterion_ids;
    std::vector<std::string> plan_node_ids;
    std::vector<std::string> artifact_ids;
};

struct ArtifactBinding {
    std::string artifact_id;
    std::string content_digest;
    std::string producer_node_id;
    std::vector<std::string> criterion_ids;
    std::vector<std::string> depends_on_artifact_ids;
    std::vector<std::string> evidence_ids;
    std::vector<std::string> memory_record_ids;
    std::vector<std::string> verifier_roles;
};

struct EvidenceBinding {
    std::string evidence_id;
    std::string criterion_id;
    std::string artifact_id;
    std::string source_kind;
    std::string content_digest;
    std::string freshness_deadline;
    bool strong_oracle{false};
};

struct ImpactInventory {
    contracts::ContractMetadata metadata;
    std::string inventory_id;
    std::string plan_digest;
    std::string artifact_manifest_digest;
    std::vector<RequirementBinding> requirements;
    std::vector<ArtifactBinding> artifacts;
    std::vector<EvidenceBinding> evidence;
};

struct FindingBinding {
    std::string finding_id;
    std::string criterion_id;
    std::vector<std::string> requirement_ids;
    std::vector<std::string> plan_node_ids;
    std::vector<std::string> artifact_ids;
    std::vector<std::string> evidence_ids;
};

struct ImpactGraph {
    contracts::ContractMetadata metadata;
    std::string graph_id;
    std::string inventory_digest;
    std::string acceptance_report_digest;
    std::vector<FindingBinding> finding_bindings;
    std::vector<std::string> affected_criterion_ids;
    std::vector<std::string> affected_plan_node_ids;
    std::vector<std::string> invalidated_artifact_ids;
    std::vector<std::string> invalidated_evidence_ids;
    std::vector<std::string> invalidated_memory_record_ids;
    std::vector<std::string> verifier_roles;
};

struct CriterionChange {
    std::string criterion_id;
    bool old_mandatory{true};
    bool new_mandatory{true};
    std::string old_threshold;
    std::string new_threshold;
    std::string rationale;
};

struct RemediationAction {
    std::string action_id;
    std::string objective;
    std::vector<std::string> finding_ids;
    std::vector<std::string> affected_plan_node_ids;
    std::vector<std::string> affected_artifact_ids;
    std::vector<std::string> required_capabilities;
    std::vector<std::string> side_effects;
    std::vector<std::string> output_contracts;
    std::string rollback_strategy;
    std::string risk_level;
    bool approval_required{false};
};

struct RemediationPlan {
    contracts::ContractMetadata metadata;
    std::string remediation_id;
    std::uint64_t revision{1};
    std::string parent_plan_digest;
    std::string acceptance_report_digest;
    std::string impact_graph_digest;
    std::vector<RemediationAction> actions;
    std::vector<CriterionChange> criterion_changes;
    std::string planner_invocation_id;
};

struct ReverificationPlan {
    contracts::ContractMetadata metadata;
    std::string reverification_id;
    std::string remediation_plan_digest;
    std::string proposed_plan_digest;
    std::vector<std::string> criterion_ids;
    std::vector<std::string> forced_oracle_kinds;
    std::vector<std::string> verifier_roles;
    std::vector<std::string> invalidated_evidence_ids;
    std::vector<std::string> reusable_evidence_ids;
    std::map<std::string, std::string> baseline_artifact_digests;
    std::string created_at;
};

struct RemediationStageArtifact {
    RemediationStage stage{RemediationStage::ImpactAnalysis};
    std::uint64_t attempt{0};
    std::string invocation_id;
    std::string output_digest;
    std::string provider;
    std::string model;
    std::uint64_t tokens{0};
    double cost_usd{0.0};
    nlohmann::json output = nlohmann::json::object();
};

struct RemediationCheckpoint {
    contracts::ContractMetadata metadata;
    std::string workflow_id;
    std::uint64_t revision{1};
    RemediationState state{RemediationState::Running};
    RemediationStage next_stage{RemediationStage::ImpactAnalysis};
    std::map<std::string, std::uint64_t> stage_attempts;
    std::vector<std::string> completed_stages;
    std::string current_plan_digest;
    std::string acceptance_contract_digest;
    std::string acceptance_report_digest;
    std::string assurance_checkpoint_digest;
    std::string impact_inventory_digest;
    std::string memory_snapshot_id;
    std::string memory_view_digest;
    std::optional<ImpactGraph> impact_graph;
    std::optional<RemediationPlan> remediation_plan;
    std::optional<planning::ExecutionPlan> proposed_plan;
    std::optional<ReverificationPlan> reverification_plan;
    std::vector<RemediationStageArtifact> artifacts;
    std::vector<std::string> proposal_signatures;
    std::uint64_t consumed_tokens{0};
    double consumed_cost_usd{0.0};
    std::string approval_request_digest;
    std::string approval_decision_id;
    std::string committed_plan_digest;
    std::string error_code;
    std::string error_message;
    std::string updated_at;
};

std::string remediation_stage_name(RemediationStage value);
std::optional<RemediationStage> remediation_stage_from_name(std::string_view value);
std::string remediation_state_name(RemediationState value);
std::optional<RemediationState> remediation_state_from_name(std::string_view value);

nlohmann::json encode(const ImpactInventory& value);
nlohmann::json encode(const ImpactGraph& value);
nlohmann::json encode(const RemediationPlan& value);
nlohmann::json encode(const ReverificationPlan& value);
nlohmann::json encode(const RemediationCheckpoint& value);
std::optional<ImpactInventory> decode_impact_inventory(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<ImpactGraph> decode_impact_graph(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<RemediationPlan> decode_remediation_plan(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<ReverificationPlan> decode_reverification_plan(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<RemediationCheckpoint> decode_remediation_checkpoint(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);

enum class RemediationStoreStatus {
    Committed, AlreadyExists, NotFound, RevisionConflict, Invalid, Busy, Error
};
struct RemediationStoreCommit {
    RemediationStoreStatus status{RemediationStoreStatus::Error};
    std::uint64_t revision{0};
    std::string digest;
    std::string error;
    explicit operator bool() const noexcept { return status == RemediationStoreStatus::Committed; }
};
struct StoredRemediationCheckpoint {
    RemediationCheckpoint checkpoint;
    std::uint64_t revision{0};
};
class RemediationStore {
public:
    virtual ~RemediationStore() = default;
    virtual RemediationStoreCommit create(const RemediationCheckpoint& value) = 0;
    virtual std::optional<StoredRemediationCheckpoint> load(
        std::string_view tenant_id, std::string_view workflow_id) = 0;
    virtual RemediationStoreCommit compare_exchange(
        const RemediationCheckpoint& value, std::uint64_t expected_revision) = 0;
};
class InMemoryRemediationStore final : public RemediationStore {
public:
    RemediationStoreCommit create(const RemediationCheckpoint& value) override;
    std::optional<StoredRemediationCheckpoint> load(
        std::string_view tenant_id, std::string_view workflow_id) override;
    RemediationStoreCommit compare_exchange(
        const RemediationCheckpoint& value, std::uint64_t expected_revision) override;
private:
    static std::string key(std::string_view tenant_id, std::string_view workflow_id);
    std::mutex mutex_;
    std::map<std::string, StoredRemediationCheckpoint> values_;
};
struct SQLiteRemediationStoreOptions {
    int busy_timeout_ms{3000};
    bool require_private_permissions{true};
};
class SQLiteRemediationStore final : public RemediationStore {
public:
    explicit SQLiteRemediationStore(std::string path, SQLiteRemediationStoreOptions options = {});
    ~SQLiteRemediationStore() override;
    SQLiteRemediationStore(const SQLiteRemediationStore&) = delete;
    SQLiteRemediationStore& operator=(const SQLiteRemediationStore&) = delete;
    RemediationStoreCommit create(const RemediationCheckpoint& value) override;
    std::optional<StoredRemediationCheckpoint> load(
        std::string_view tenant_id, std::string_view workflow_id) override;
    RemediationStoreCommit compare_exchange(
        const RemediationCheckpoint& value, std::uint64_t expected_revision) override;
private:
    void migrate();
    void* db_{nullptr};
    std::string path_;
    SQLiteRemediationStoreOptions options_;
    std::mutex mutex_;
};

struct RemediationRoleBinding {
    std::string profile_id;
    std::string profile_revision;
    std::vector<std::string> granted_capabilities;
    std::string required_region;
};
struct RemediationStageRequest {
    contracts::ContractMetadata metadata;
    std::string workflow_id;
    RemediationStage stage{RemediationStage::ImpactAnalysis};
    std::uint64_t attempt{0};
    memory_v2::MemoryView memory_view;
    nlohmann::json input = nlohmann::json::object();
    llm_runtime::IndependenceRequirement independence;
    std::vector<std::string> granted_capabilities;
    std::function<bool()> cancelled;
};
struct RemediationStageResponse {
    bool ok{false};
    nlohmann::json output = nlohmann::json::object();
    llm_runtime::LLMInvocationManifest manifest;
    std::string error_code;
    std::string error_message;
};
class RemediationStageModel {
public:
    virtual ~RemediationStageModel() = default;
    virtual RemediationStageResponse invoke(const RemediationStageRequest& request) = 0;
};
class RoleRuntimeRemediationModel final : public RemediationStageModel {
public:
    explicit RoleRuntimeRemediationModel(std::shared_ptr<llm_runtime::RoleRuntime> runtime);
    bool bind(RemediationStage stage, RemediationRoleBinding binding);
    RemediationStageResponse invoke(const RemediationStageRequest& request) override;
private:
    std::shared_ptr<llm_runtime::RoleRuntime> runtime_;
    std::map<RemediationStage, RemediationRoleBinding> bindings_;
};

struct RemediationWorkflowOptions {
    std::string workflow_id;
    std::string deadline;
    std::uint64_t max_iterations{2};
    std::uint64_t token_budget{100000};
    double cost_limit_usd{10.0};
    std::vector<std::string> allowed_capabilities;
    std::string actor_id;
    std::vector<std::string> actor_roles;
    std::string approval_decision_id;
    std::function<bool(std::string_view request_digest, std::string_view decision_id)>
        approval_validator;
    std::function<bool()> cancelled;
    std::function<std::string()> now;
    std::function<void()> after_plan_commit;
};
struct RemediationWorkflowResult {
    RemediationState state{RemediationState::Failed};
    RemediationCheckpoint checkpoint;
    std::optional<ImpactGraph> impact_graph;
    std::optional<RemediationPlan> remediation_plan;
    std::optional<planning::ExecutionPlan> proposed_plan;
    std::optional<ReverificationPlan> reverification_plan;
    std::string error_code;
    std::string error_message;
};

class LLMRemediationWorkflow {
public:
    LLMRemediationWorkflow(memory_v2::MemoryViewEngine& views,
                           RemediationStore& store,
                           planning::PlanStore& plans,
                           RemediationStageModel& model,
                           approval::PolicyDecisionPoint policy =
                               approval::PolicyDecisionPoint(approval::PolicyRules{}));
    RemediationWorkflowResult run(
        const planning::ExecutionPlan& current_plan,
        const assurance::AcceptanceContract& contract,
        const assurance::AcceptanceReport& report,
        const assurance::AssuranceCheckpoint& assurance_checkpoint,
        const ImpactInventory& inventory,
        const memory_v2::MemoryScope& subject,
        const RemediationWorkflowOptions& options = {});
private:
    memory_v2::MemoryViewEngine& views_;
    RemediationStore& store_;
    planning::PlanStore& plans_;
    RemediationStageModel& model_;
    approval::PolicyDecisionPoint policy_;
};

}  // namespace agent_framework::remediation
