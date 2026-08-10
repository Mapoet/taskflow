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
#include "agent/eval/runner.hpp"
#include "agent/llm_runtime/runtime.hpp"
#include "agent/memory_v2/view_engine.hpp"
#include "agent/memory_v2/view_profiles.hpp"

namespace agent_framework::eval {

enum class DatasetLayer { Unit, Repository, Domain, Adversarial, Recovery, Live };
enum class JudgeStage {
    Preparation,
    PrimaryJudging,
    SecondaryJudging,
    Calibration,
    Adjudication,
    Metrics,
    UpgradeGate,
    Complete
};
enum class JudgeWorkflowState {
    Running,
    AwaitingApproval,
    Approved,
    Rejected,
    ManualReview,
    Failed,
    Cancelled
};
enum class UpgradeOutcome { AwaitingApproval, Approved, Rejected, ManualReview };

struct SuiteCase {
    std::string case_id;
    DatasetLayer layer{DatasetLayer::Unit};
    double weight{1.0};
    std::vector<std::string> criterion_ids;
};

struct MetricPolicy {
    std::string metric;
    DatasetLayer layer{DatasetLayer::Unit};
    bool higher_is_better{true};
    double maximum_regression{0.0};
    std::optional<double> minimum_candidate_value;
    bool critical_zero{false};
};

struct EvaluationSuite {
    contracts::ContractMetadata metadata;
    std::string suite_id;
    std::uint64_t revision{1};
    std::string dataset_version;
    std::uint64_t seed{0};
    std::vector<SuiteCase> cases;
    std::vector<MetricPolicy> metric_policies;
    double minimum_judge_agreement{0.7};
    double minimum_ground_truth_accuracy{0.7};
    double maximum_flaky_delta{0.05};
    bool requires_live_execution{false};
};

struct EvaluationCaseRun {
    std::string case_id;
    Trajectory trajectory;
    nlohmann::json artifact = nlohmann::json::object();
    std::map<std::string, double> metrics;
    std::map<std::string, std::vector<double>> metric_samples;
    std::string error;
};

struct CandidateEvaluationRun {
    contracts::ContractMetadata metadata;
    std::string run_id;
    std::string revision_id;
    std::string profile_revision;
    std::string prompt_revision;
    std::string model_revision;
    bool executed{false};
    std::vector<EvaluationCaseRun> cases;
    std::string started_at;
    std::string finished_at;
};

struct BlindAssignment {
    std::string case_id;
    std::string baseline_alias;
    std::string candidate_alias;
    std::vector<std::string> primary_order;
    std::vector<std::string> secondary_order;
    std::string baseline_artifact_digest;
    std::string candidate_artifact_digest;
};

struct JudgeVerdict {
    std::string case_id;
    std::string winner_alias;
    std::map<std::string, double> scores;
    std::map<std::string, double> criterion_scores;
    double confidence{0.0};
    std::vector<std::string> evidence_refs;
    std::vector<std::string> risks;
};

struct JudgeBatch {
    contracts::ContractMetadata metadata;
    std::string batch_id;
    std::string judge_role;
    std::string invocation_id;
    std::string independence_group;
    std::string provider;
    std::string model;
    std::vector<JudgeVerdict> verdicts;
};

struct JudgeCalibrationReport {
    contracts::ContractMetadata metadata;
    std::string calibration_id;
    double raw_agreement{0.0};
    double cohens_kappa{0.0};
    double first_position_win_rate{0.0};
    double score_delta_variance{0.0};
    double ground_truth_accuracy{0.0};
    std::uint64_t ground_truth_samples{0};
    double candidate_win_rate{0.0};
    double candidate_win_ci95_low{0.0};
    double candidate_win_ci95_high{0.0};
    std::vector<std::string> disputed_case_ids;
    std::vector<std::string> calibration_findings;
};

struct QualityMetricReport {
    contracts::ContractMetadata metadata;
    std::string report_id;
    std::vector<MetricComparison> comparisons;
    std::vector<std::string> missing_metrics;
    std::vector<std::string> flaky_metrics;
    std::vector<std::string> critical_violations;
    std::map<std::string, double> candidate_means;
};

struct UpgradeDecision {
    contracts::ContractMetadata metadata;
    std::string decision_id;
    UpgradeOutcome outcome{UpgradeOutcome::ManualReview};
    std::string baseline_revision;
    std::string candidate_revision;
    std::string rollback_revision;
    std::string calibration_digest;
    std::string metric_report_digest;
    std::vector<std::string> reasons;
    std::string approval_request_digest;
    std::string approval_decision_id;
};

struct EvaluationReport {
    contracts::ContractMetadata metadata;
    std::string workflow_id;
    std::string run_kind;
    bool executed{false};
    std::string suite_digest;
    std::string baseline_run_digest;
    std::string candidate_run_digest;
    JudgeBatch primary;
    JudgeBatch secondary;
    std::optional<JudgeBatch> adjudication;
    JudgeCalibrationReport calibration;
    QualityMetricReport metrics;
    UpgradeDecision decision;
    std::string trend_key;
    std::string created_at;
};

struct JudgeStageArtifact {
    JudgeStage stage{JudgeStage::PrimaryJudging};
    std::uint64_t attempt{0};
    std::string invocation_id;
    std::string output_digest;
    std::string provider;
    std::string model;
    std::string independence_group;
    std::uint64_t tokens{0};
    double cost_usd{0.0};
    nlohmann::json output = nlohmann::json::object();
};

struct JudgeCheckpoint {
    contracts::ContractMetadata metadata;
    std::string workflow_id;
    std::uint64_t revision{1};
    JudgeWorkflowState state{JudgeWorkflowState::Running};
    JudgeStage next_stage{JudgeStage::Preparation};
    std::map<std::string, std::uint64_t> stage_attempts;
    std::vector<std::string> completed_stages;
    std::string suite_digest;
    std::string baseline_run_digest;
    std::string candidate_run_digest;
    std::string memory_snapshot_id;
    std::string memory_view_digest;
    std::vector<BlindAssignment> assignments;
    std::optional<JudgeBatch> primary;
    std::optional<JudgeBatch> secondary;
    std::optional<JudgeBatch> adjudication;
    std::optional<JudgeCalibrationReport> calibration;
    std::optional<QualityMetricReport> metrics;
    std::optional<UpgradeDecision> decision;
    std::vector<JudgeStageArtifact> artifacts;
    std::uint64_t consumed_tokens{0};
    double consumed_cost_usd{0.0};
    std::string evaluation_report_digest;
    std::string error_code;
    std::string error_message;
    std::string updated_at;
};

std::string dataset_layer_name(DatasetLayer value);
std::optional<DatasetLayer> dataset_layer_from_name(std::string_view value);
std::string judge_stage_name(JudgeStage value);
std::optional<JudgeStage> judge_stage_from_name(std::string_view value);
std::string judge_workflow_state_name(JudgeWorkflowState value);
std::optional<JudgeWorkflowState> judge_workflow_state_from_name(std::string_view value);
std::string upgrade_outcome_name(UpgradeOutcome value);
std::optional<UpgradeOutcome> upgrade_outcome_from_name(std::string_view value);

nlohmann::json encode(const EvaluationSuite& value);
nlohmann::json encode(const CandidateEvaluationRun& value);
nlohmann::json encode(const JudgeBatch& value);
nlohmann::json encode(const JudgeCalibrationReport& value);
nlohmann::json encode(const QualityMetricReport& value);
nlohmann::json encode(const UpgradeDecision& value);
nlohmann::json encode(const EvaluationReport& value);
nlohmann::json encode(const JudgeCheckpoint& value);
std::optional<EvaluationSuite> decode_evaluation_suite(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<CandidateEvaluationRun> decode_candidate_evaluation_run(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<EvaluationReport> decode_evaluation_report(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<JudgeCheckpoint> decode_judge_checkpoint(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);

enum class JudgeStoreStatus {
    Committed, AlreadyExists, NotFound, RevisionConflict, Invalid, Busy, Error
};
struct JudgeStoreCommit {
    JudgeStoreStatus status{JudgeStoreStatus::Error};
    std::uint64_t revision{0};
    std::string digest;
    std::string error;
    explicit operator bool() const noexcept { return status == JudgeStoreStatus::Committed; }
};
struct StoredJudgeCheckpoint { JudgeCheckpoint checkpoint; std::uint64_t revision{0}; };
struct StoredEvaluationReport { EvaluationReport report; std::uint64_t revision{0}; };
class JudgeStore {
public:
    virtual ~JudgeStore() = default;
    virtual JudgeStoreCommit create(const JudgeCheckpoint& value) = 0;
    virtual std::optional<StoredJudgeCheckpoint> load(
        std::string_view tenant_id, std::string_view workflow_id) = 0;
    virtual JudgeStoreCommit compare_exchange(
        const JudgeCheckpoint& value, std::uint64_t expected_revision) = 0;
    virtual JudgeStoreCommit commit_report(
        const JudgeCheckpoint& terminal, std::uint64_t expected_revision,
        const EvaluationReport& report) = 0;
    virtual std::optional<StoredEvaluationReport> load_report(
        std::string_view tenant_id, std::string_view workflow_id) = 0;
};
class InMemoryJudgeStore final : public JudgeStore {
public:
    JudgeStoreCommit create(const JudgeCheckpoint& value) override;
    std::optional<StoredJudgeCheckpoint> load(
        std::string_view tenant_id, std::string_view workflow_id) override;
    JudgeStoreCommit compare_exchange(
        const JudgeCheckpoint& value, std::uint64_t expected_revision) override;
    JudgeStoreCommit commit_report(
        const JudgeCheckpoint& terminal, std::uint64_t expected_revision,
        const EvaluationReport& report) override;
    std::optional<StoredEvaluationReport> load_report(
        std::string_view tenant_id, std::string_view workflow_id) override;
private:
    static std::string key(std::string_view tenant_id, std::string_view workflow_id);
    std::mutex mutex_;
    std::map<std::string, StoredJudgeCheckpoint> checkpoints_;
    std::map<std::string, StoredEvaluationReport> reports_;
};
struct SQLiteJudgeStoreOptions { int busy_timeout_ms{3000}; bool require_private_permissions{true}; };
class SQLiteJudgeStore final : public JudgeStore {
public:
    explicit SQLiteJudgeStore(std::string path, SQLiteJudgeStoreOptions options = {});
    ~SQLiteJudgeStore() override;
    SQLiteJudgeStore(const SQLiteJudgeStore&) = delete;
    SQLiteJudgeStore& operator=(const SQLiteJudgeStore&) = delete;
    JudgeStoreCommit create(const JudgeCheckpoint& value) override;
    std::optional<StoredJudgeCheckpoint> load(
        std::string_view tenant_id, std::string_view workflow_id) override;
    JudgeStoreCommit compare_exchange(
        const JudgeCheckpoint& value, std::uint64_t expected_revision) override;
    JudgeStoreCommit commit_report(
        const JudgeCheckpoint& terminal, std::uint64_t expected_revision,
        const EvaluationReport& report) override;
    std::optional<StoredEvaluationReport> load_report(
        std::string_view tenant_id, std::string_view workflow_id) override;
private:
    void migrate();
    void* db_{nullptr};
    std::string path_;
    SQLiteJudgeStoreOptions options_;
    std::mutex mutex_;
};

struct JudgeRoleBinding {
    std::string profile_id;
    std::string profile_revision;
    std::vector<std::string> granted_capabilities;
    std::string required_region;
};
struct JudgeStageRequest {
    contracts::ContractMetadata metadata;
    std::string workflow_id;
    JudgeStage stage{JudgeStage::PrimaryJudging};
    std::uint64_t attempt{0};
    memory_v2::MemoryView memory_view;
    nlohmann::json input = nlohmann::json::object();
    llm_runtime::IndependenceRequirement independence;
    std::vector<std::string> granted_capabilities;
    std::function<bool()> cancelled;
};
struct JudgeStageResponse {
    bool ok{false};
    nlohmann::json output = nlohmann::json::object();
    llm_runtime::LLMInvocationManifest manifest;
    std::string error_code;
    std::string error_message;
};
class JudgeStageModel {
public:
    virtual ~JudgeStageModel() = default;
    virtual JudgeStageResponse invoke(const JudgeStageRequest& request) = 0;
};
class RoleRuntimeJudgeModel final : public JudgeStageModel {
public:
    explicit RoleRuntimeJudgeModel(std::shared_ptr<llm_runtime::RoleRuntime> runtime);
    bool bind(JudgeStage stage, JudgeRoleBinding binding);
    JudgeStageResponse invoke(const JudgeStageRequest& request) override;
private:
    std::shared_ptr<llm_runtime::RoleRuntime> runtime_;
    std::map<JudgeStage, JudgeRoleBinding> bindings_;
};

struct JudgeWorkflowOptions {
    std::string workflow_id;
    std::string run_kind{"manual"};
    std::string deadline;
    std::uint64_t max_stage_attempts{2};
    std::uint64_t token_budget{200000};
    double cost_limit_usd{20.0};
    bool require_upgrade_approval{true};
    std::string approval_decision_id;
    std::function<bool(std::string_view request_digest, std::string_view decision_id)>
        approval_validator;
    std::vector<std::string> forbidden_independence_groups;
    std::vector<std::string> forbidden_providers;
    std::vector<std::string> forbidden_models;
    bool require_provider_diversity{true};
    bool require_model_diversity{true};
    std::function<bool()> cancelled;
    std::function<std::string()> now;
};
struct JudgeWorkflowResult {
    JudgeWorkflowState state{JudgeWorkflowState::Failed};
    JudgeCheckpoint checkpoint;
    std::optional<EvaluationReport> report;
    std::string error_code;
    std::string error_message;
};

class LLMJudgeWorkflow {
public:
    LLMJudgeWorkflow(memory_v2::MemoryViewEngine& views,
                     JudgeStore& store,
                     JudgeStageModel& model,
                     approval::PolicyDecisionPoint policy =
                         approval::PolicyDecisionPoint(approval::PolicyRules{}));
    JudgeWorkflowResult run(const EvaluationSuite& suite,
                            const DatasetRegistry& datasets,
                            const CandidateEvaluationRun& baseline,
                            const CandidateEvaluationRun& candidate,
                            const memory_v2::MemoryScope& subject,
                            const JudgeWorkflowOptions& options = {});
private:
    memory_v2::MemoryViewEngine& views_;
    JudgeStore& store_;
    JudgeStageModel& model_;
    approval::PolicyDecisionPoint policy_;
};

}  // namespace agent_framework::eval
