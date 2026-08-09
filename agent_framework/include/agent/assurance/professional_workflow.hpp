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

#include "agent/assurance/arbiter.hpp"
#include "agent/llm_runtime/runtime.hpp"
#include "agent/memory_v2/view_engine.hpp"
#include "agent/memory_v2/view_profiles.hpp"

namespace agent_framework::assurance {

enum class ProfessionalRole {
    VerificationPlanner,
    Code,
    Architecture,
    Domain,
    Security,
    Completeness,
    EvidenceResolver
};

enum class AssuranceStage {
    Planning,
    DeterministicEvidence,
    CodeVerification,
    ArchitectureVerification,
    DomainVerification,
    SecurityVerification,
    CompletenessVerification,
    EvidenceResolution,
    Arbitration,
    Complete
};

enum class AssuranceWorkflowState {
    Running,
    Completed,
    Failed,
    Cancelled,
    ManualReview
};

struct VerificationAssignment {
    std::string assignment_id;
    ProfessionalRole role{ProfessionalRole::Code};
    std::vector<std::string> criterion_ids;
    std::vector<std::string> required_evidence;
    std::vector<std::string> granted_capabilities;
    bool read_only{true};
    bool mandatory{true};
    std::string rationale;
};

struct VerificationPlan {
    contracts::ContractMetadata metadata;
    std::string verification_plan_id;
    std::uint64_t revision{1};
    std::string acceptance_contract_digest;
    std::string task_plan_digest;
    std::string artifact_manifest_digest;
    std::vector<VerificationAssignment> assignments;
    std::string planner_invocation_id;
    std::string created_at;
};

struct AssuranceStageArtifact {
    AssuranceStage stage{AssuranceStage::Planning};
    std::uint64_t attempt{0};
    std::string invocation_id;
    std::string manifest_digest;
    std::string output_digest;
    std::string provider;
    std::string model;
    std::string independence_group;
    std::string calibration_revision;
    nlohmann::json output = nlohmann::json::object();
};

struct EvidenceConflict {
    std::string conflict_id;
    std::string criterion_id;
    std::vector<std::string> evidence_ids;
    std::string conflict_kind;
    bool resolved{false};
    std::string resolution;
};

struct ResolutionReport {
    contracts::ContractMetadata metadata;
    std::string workflow_id;
    std::vector<Finding> normalized_findings;
    std::vector<EvidenceConflict> conflicts;
    std::vector<std::string> residual_risks;
    std::string resolver_invocation_id;
};

struct AssuranceCheckpoint {
    contracts::ContractMetadata metadata;
    std::string workflow_id;
    std::uint64_t revision{1};
    AssuranceWorkflowState state{AssuranceWorkflowState::Running};
    AssuranceStage next_stage{AssuranceStage::Planning};
    std::map<std::string, std::uint64_t> stage_attempts;
    std::vector<std::string> completed_stages;
    std::string acceptance_contract_digest;
    std::string task_context_digest;
    std::string artifact_manifest_digest;
    std::string memory_snapshot_id;
    std::string memory_view_digest;
    std::optional<VerificationPlan> verification_plan;
    std::vector<VerificationEvidence> evidence;
    std::vector<Finding> findings;
    std::vector<AssuranceStageArtifact> artifacts;
    std::optional<ResolutionReport> resolution;
    std::string acceptance_report_digest;
    std::string error_code;
    std::string error_message;
    std::string updated_at;
};

std::string professional_role_name(ProfessionalRole value);
std::optional<ProfessionalRole> professional_role_from_name(std::string_view value);
std::string assurance_stage_name(AssuranceStage value);
std::optional<AssuranceStage> assurance_stage_from_name(std::string_view value);
std::string assurance_workflow_state_name(AssuranceWorkflowState value);
std::optional<AssuranceWorkflowState> assurance_workflow_state_from_name(std::string_view value);

nlohmann::json encode(const VerificationPlan& value);
nlohmann::json encode(const ResolutionReport& value);
nlohmann::json encode(const AssuranceCheckpoint& value);
std::optional<VerificationPlan> decode_verification_plan(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<ResolutionReport> decode_resolution_report(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<AssuranceCheckpoint> decode_assurance_checkpoint(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);

enum class AssuranceStoreStatus {
    Committed,
    AlreadyExists,
    NotFound,
    RevisionConflict,
    Invalid,
    Busy,
    Error
};

struct AssuranceStoreCommit {
    AssuranceStoreStatus status{AssuranceStoreStatus::Error};
    std::uint64_t revision{0};
    std::string digest;
    std::string error;
    explicit operator bool() const noexcept { return status == AssuranceStoreStatus::Committed; }
};

struct StoredAssuranceCheckpoint {
    AssuranceCheckpoint checkpoint;
    std::uint64_t revision{0};
};

struct StoredAcceptanceReport {
    AcceptanceReport report;
    std::uint64_t revision{0};
    std::string workflow_id;
};

class AssuranceStore {
public:
    virtual ~AssuranceStore() = default;
    virtual AssuranceStoreCommit create_checkpoint(const AssuranceCheckpoint& checkpoint) = 0;
    virtual std::optional<StoredAssuranceCheckpoint> load_checkpoint(
        std::string_view tenant_id, std::string_view workflow_id) = 0;
    virtual AssuranceStoreCommit compare_exchange_checkpoint(
        const AssuranceCheckpoint& checkpoint, std::uint64_t expected_revision) = 0;
    virtual AssuranceStoreCommit commit_report(
        const AssuranceCheckpoint& terminal_checkpoint, std::uint64_t expected_revision,
        const AcceptanceReport& report) = 0;
    virtual std::optional<StoredAcceptanceReport> load_report(
        std::string_view tenant_id, std::string_view workflow_id) = 0;
};

class InMemoryAssuranceStore final : public AssuranceStore {
public:
    AssuranceStoreCommit create_checkpoint(const AssuranceCheckpoint& checkpoint) override;
    std::optional<StoredAssuranceCheckpoint> load_checkpoint(
        std::string_view tenant_id, std::string_view workflow_id) override;
    AssuranceStoreCommit compare_exchange_checkpoint(
        const AssuranceCheckpoint& checkpoint, std::uint64_t expected_revision) override;
    AssuranceStoreCommit commit_report(
        const AssuranceCheckpoint& terminal_checkpoint, std::uint64_t expected_revision,
        const AcceptanceReport& report) override;
    std::optional<StoredAcceptanceReport> load_report(
        std::string_view tenant_id, std::string_view workflow_id) override;

private:
    static std::string key(std::string_view tenant_id, std::string_view workflow_id);
    std::mutex mutex_;
    std::map<std::string, StoredAssuranceCheckpoint> checkpoints_;
    std::map<std::string, StoredAcceptanceReport> reports_;
};

struct SQLiteAssuranceStoreOptions {
    int busy_timeout_ms{3000};
    bool require_private_permissions{true};
};

class SQLiteAssuranceStore final : public AssuranceStore {
public:
    explicit SQLiteAssuranceStore(std::string path, SQLiteAssuranceStoreOptions options = {});
    ~SQLiteAssuranceStore() override;
    SQLiteAssuranceStore(const SQLiteAssuranceStore&) = delete;
    SQLiteAssuranceStore& operator=(const SQLiteAssuranceStore&) = delete;

    AssuranceStoreCommit create_checkpoint(const AssuranceCheckpoint& checkpoint) override;
    std::optional<StoredAssuranceCheckpoint> load_checkpoint(
        std::string_view tenant_id, std::string_view workflow_id) override;
    AssuranceStoreCommit compare_exchange_checkpoint(
        const AssuranceCheckpoint& checkpoint, std::uint64_t expected_revision) override;
    AssuranceStoreCommit commit_report(
        const AssuranceCheckpoint& terminal_checkpoint, std::uint64_t expected_revision,
        const AcceptanceReport& report) override;
    std::optional<StoredAcceptanceReport> load_report(
        std::string_view tenant_id, std::string_view workflow_id) override;
    const std::string& path() const noexcept { return path_; }

private:
    void migrate();
    void* db_{nullptr};
    std::string path_;
    SQLiteAssuranceStoreOptions options_;
    std::mutex mutex_;
};

struct OracleContext {
    contracts::ContractMetadata metadata;
    VerificationPlan plan;
    AcceptanceContract contract;
    nlohmann::json artifact_manifest = nlohmann::json::object();
    std::string now;
};

struct OracleResult {
    std::vector<VerificationEvidence> evidence;
    std::vector<Finding> findings;
    std::string error;
};

class DeterministicOracle {
public:
    virtual ~DeterministicOracle() = default;
    virtual std::string id() const = 0;
    virtual std::vector<std::string> source_kinds() const = 0;
    virtual OracleResult collect(const OracleContext& context) = 0;
};

class OracleRegistry {
public:
    bool register_oracle(std::shared_ptr<DeterministicOracle> oracle);
    std::vector<std::shared_ptr<DeterministicOracle>> all() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<DeterministicOracle>> oracles_;
};

// Converts trusted, digest-bound observation manifests into deterministic/real-system
// evidence. It never executes commands and never treats model output as an oracle.
class ManifestEvidenceOracle final : public DeterministicOracle {
public:
    ManifestEvidenceOracle(std::string oracle_id, std::vector<std::string> source_kinds);
    std::string id() const override { return oracle_id_; }
    std::vector<std::string> source_kinds() const override { return source_kinds_; }
    OracleResult collect(const OracleContext& context) override;

private:
    std::string oracle_id_;
    std::vector<std::string> source_kinds_;
};

struct AssuranceRoleBinding {
    std::string profile_id;
    std::string profile_revision;
    std::vector<std::string> granted_capabilities;
    std::string required_region;
};

struct AssuranceStageRequest {
    contracts::ContractMetadata metadata;
    std::string workflow_id;
    AssuranceStage stage{AssuranceStage::Planning};
    std::uint64_t attempt{0};
    memory_v2::MemoryView memory_view;
    nlohmann::json input = nlohmann::json::object();
    llm_runtime::IndependenceRequirement independence;
    std::vector<std::string> granted_capabilities;
    std::function<bool()> cancelled;
};

struct AssuranceStageResponse {
    bool ok{false};
    nlohmann::json output = nlohmann::json::object();
    llm_runtime::LLMInvocationManifest manifest;
    std::string error_code;
    std::string error_message;
};

class AssuranceStageModel {
public:
    virtual ~AssuranceStageModel() = default;
    virtual AssuranceStageResponse invoke(const AssuranceStageRequest& request) = 0;
};

class RoleRuntimeAssuranceModel final : public AssuranceStageModel {
public:
    explicit RoleRuntimeAssuranceModel(std::shared_ptr<llm_runtime::RoleRuntime> runtime);
    bool bind(AssuranceStage stage, AssuranceRoleBinding binding);
    AssuranceStageResponse invoke(const AssuranceStageRequest& request) override;

private:
    std::shared_ptr<llm_runtime::RoleRuntime> runtime_;
    std::map<AssuranceStage, AssuranceRoleBinding> bindings_;
};

struct AssuranceWorkflowEvent {
    std::string workflow_id;
    std::uint64_t checkpoint_revision{0};
    AssuranceStage stage{AssuranceStage::Planning};
    std::string event_type;
    nlohmann::json payload = nlohmann::json::object();
};

struct AssuranceWorkflowOptions {
    std::string workflow_id;
    std::string deadline;
    std::uint64_t max_stage_attempts{2};
    std::vector<std::string> forbidden_independence_groups;
    std::vector<std::string> forbidden_providers;
    std::vector<std::string> forbidden_models;
    bool require_provider_diversity{false};
    bool require_model_diversity{false};
    std::function<bool()> cancelled;
    std::function<std::string()> now;
    std::function<void(const AssuranceWorkflowEvent&)> event_sink;
};

struct AssuranceWorkflowResult {
    AssuranceWorkflowState state{AssuranceWorkflowState::Failed};
    AssuranceCheckpoint checkpoint;
    std::optional<VerificationPlan> verification_plan;
    std::optional<ResolutionReport> resolution;
    std::optional<AcceptanceReport> report;
    std::string error_code;
    std::string error_message;
};

class ProfessionalAssuranceWorkflow {
public:
    ProfessionalAssuranceWorkflow(memory_v2::MemoryViewEngine& views,
                                  OracleRegistry& oracles,
                                  AssuranceStore& store,
                                  AssuranceStageModel& model,
                                  AcceptanceArbiter arbiter = {});

    AssuranceWorkflowResult run(const AcceptanceContract& contract,
                                const memory_v2::MemoryScope& subject,
                                const nlohmann::json& task_context,
                                const nlohmann::json& artifact_manifest,
                                const AssuranceWorkflowOptions& options = {});

private:
    memory_v2::MemoryViewEngine& views_;
    OracleRegistry& oracles_;
    AssuranceStore& store_;
    AssuranceStageModel& model_;
    AcceptanceArbiter arbiter_;
};

}  // namespace agent_framework::assurance
