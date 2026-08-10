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

#include "agent/contracts/contract.hpp"
#include "agent/llm_runtime/runtime.hpp"

namespace agent_framework::live {

enum class LiveCellKind { Cognition, Memory, Execution, Assurance, Judge, FailureRecovery };
enum class LiveCellOutcome { Passed, Failed, Inconclusive };
enum class LiveFailureScenario { None, Timeout, RateLimit, MalformedOutput, ProviderFallback, Restart, CostCap };
enum class RoleCertificationState { Running, Inconclusive, AwaitingApproval, Certified, Rejected, Failed, Cancelled };

struct ProviderProfileManifest {
    std::string role;
    std::string stage;
    std::string profile_id;
    std::string profile_revision;
    std::string prompt_id;
    std::string prompt_revision;
    std::string provider;
    std::string model;
    std::string adapter_revision;
    std::string model_family;
    std::string independence_group;
    std::string region;
    std::string config_digest;
    std::string calibration_id;
    std::string calibration_digest;
    bool calibration_approved{false};
    std::vector<std::string> capabilities;
    std::vector<std::string> dependency_digests;
};

struct LiveEnvironmentProfile {
    contracts::ContractMetadata metadata;
    std::string certification_id;
    std::uint64_t revision{1};
    std::string os;
    std::string build_digest;
    std::string git_revision;
    std::string config_digest;
    std::string provider_registry_digest;
    std::string memory_policy_digest;
    std::string sandbox_policy_digest;
    std::string telemetry_policy_digest;
    std::string evaluation_suite_digest;
    std::string endpoint_class;
    std::string region;
    std::vector<std::string> dependency_digests;
    std::vector<std::string> secret_refs;
    std::vector<ProviderProfileManifest> role_profiles;
    std::string scheduled_at;
    std::string expires_at;
};

struct LiveCellSpec {
    std::string cell_id;
    LiveCellKind kind{LiveCellKind::Cognition};
    std::string role;
    std::string stage;
    bool required{true};
    std::vector<std::string> dependencies;
    std::string profile_id;
    std::string profile_revision;
    std::string prompt_id;
    std::string prompt_revision;
    std::string provider;
    std::string model;
    std::string independence_group;
    std::string region;
    std::vector<std::string> required_capabilities;
    bool read_only{false};
    bool blind{false};
    bool strong_oracle_required{false};
    LiveFailureScenario failure_scenario{LiveFailureScenario::None};
    std::uint64_t max_latency_ms{0};
    std::uint64_t max_tokens{0};
    double max_cost_usd{0.0};
};

struct RoleLiveMatrix {
    contracts::ContractMetadata metadata;
    std::string matrix_id;
    std::uint64_t revision{1};
    std::string environment_digest;
    std::vector<LiveCellSpec> cells;
    std::uint64_t minimum_production_combinations{1};
};

struct LiveCellResult {
    std::string cell_id;
    std::string spec_digest;
    bool executed{false};
    LiveCellOutcome outcome{LiveCellOutcome::Inconclusive};
    std::string reason;
    std::string error_class;
    std::string invocation_id;
    std::string invocation_manifest_digest;
    std::string role;
    std::string profile_id;
    std::string profile_revision;
    std::string prompt_id;
    std::string prompt_revision;
    std::string provider;
    std::string model;
    std::string independence_group;
    std::string region;
    std::vector<std::string> capabilities;
    std::vector<std::string> evidence_digests;
    std::vector<std::string> oracle_digests;
    bool read_only{false};
    bool blind{false};
    bool recovered{false};
    bool fallback_used{false};
    bool unauthorized_memory_promotion{false};
    std::uint64_t latency_ms{0};
    std::uint64_t tokens{0};
    double cost_usd{0.0};
    std::string started_at;
    std::string finished_at;
};

struct SignatureEnvelope {
    std::string algorithm;
    std::string key_id;
    std::string signed_digest;
    std::string signature;
};

struct RoleCertificationReport {
    contracts::ContractMetadata metadata;
    std::string workflow_id;
    bool executed{false};
    RoleCertificationState state{RoleCertificationState::Inconclusive};
    std::string environment_digest;
    std::string matrix_digest;
    std::vector<LiveCellResult> cells;
    std::map<std::string, double> metrics;
    std::vector<std::string> findings;
    std::vector<std::string> blockers;
    std::vector<std::string> residual_risks;
    std::string approval_decision_id;
    std::string issued_at;
    std::string expires_at;
    SignatureEnvelope signature;
};

struct RoleCertificationCheckpoint {
    contracts::ContractMetadata metadata;
    std::string workflow_id;
    std::uint64_t revision{1};
    RoleCertificationState state{RoleCertificationState::Running};
    std::string environment_digest;
    std::string matrix_digest;
    std::size_t next_cell{0};
    std::vector<LiveCellResult> cells;
    std::vector<std::string> blockers;
    std::string report_digest;
    std::string error_code;
    std::string error_message;
    std::string updated_at;
};

std::string live_cell_kind_name(LiveCellKind value);
std::string live_cell_outcome_name(LiveCellOutcome value);
std::string live_failure_scenario_name(LiveFailureScenario value);
std::string role_certification_state_name(RoleCertificationState value);
std::string role_environment_digest(const LiveEnvironmentProfile& value);
std::string live_cell_spec_digest(const LiveCellSpec& value);
std::string role_live_matrix_digest(const RoleLiveMatrix& value);
std::string role_report_signing_digest(const RoleCertificationReport& value);

nlohmann::json encode(const LiveEnvironmentProfile& value);
nlohmann::json encode(const RoleLiveMatrix& value);
nlohmann::json encode(const LiveCellResult& value);
nlohmann::json encode(const RoleCertificationReport& value);
nlohmann::json encode(const RoleCertificationCheckpoint& value);
std::optional<LiveEnvironmentProfile> decode_live_environment_profile(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<RoleLiveMatrix> decode_role_live_matrix(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<RoleCertificationReport> decode_role_certification_report(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<RoleCertificationCheckpoint> decode_role_certification_checkpoint(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);

enum class RoleCertificationStoreStatus { Committed, AlreadyExists, NotFound, RevisionConflict, Invalid, Busy, Error };
struct RoleCertificationStoreCommit {
    RoleCertificationStoreStatus status{RoleCertificationStoreStatus::Error};
    std::uint64_t revision{0};
    std::string digest;
    std::string error;
    explicit operator bool() const noexcept { return status == RoleCertificationStoreStatus::Committed; }
};
struct StoredRoleCertificationCheckpoint { RoleCertificationCheckpoint checkpoint; std::uint64_t revision{0}; };
struct StoredRoleCertificationReport { RoleCertificationReport report; std::uint64_t revision{0}; };

class RoleCertificationStore {
public:
    virtual ~RoleCertificationStore() = default;
    virtual RoleCertificationStoreCommit create(const RoleCertificationCheckpoint& value) = 0;
    virtual std::optional<StoredRoleCertificationCheckpoint> load(
        std::string_view tenant_id, std::string_view workflow_id) = 0;
    virtual RoleCertificationStoreCommit compare_exchange(
        const RoleCertificationCheckpoint& value, std::uint64_t expected_revision) = 0;
    virtual RoleCertificationStoreCommit commit_report(
        const RoleCertificationCheckpoint& terminal, std::uint64_t expected_revision,
        const RoleCertificationReport& report) = 0;
    virtual std::optional<StoredRoleCertificationReport> load_report(
        std::string_view tenant_id, std::string_view workflow_id) = 0;
};

class InMemoryRoleCertificationStore final : public RoleCertificationStore {
public:
    RoleCertificationStoreCommit create(const RoleCertificationCheckpoint& value) override;
    std::optional<StoredRoleCertificationCheckpoint> load(std::string_view tenant_id,
                                                           std::string_view workflow_id) override;
    RoleCertificationStoreCommit compare_exchange(const RoleCertificationCheckpoint& value,
                                                   std::uint64_t expected_revision) override;
    RoleCertificationStoreCommit commit_report(const RoleCertificationCheckpoint& terminal,
                                                std::uint64_t expected_revision,
                                                const RoleCertificationReport& report) override;
    std::optional<StoredRoleCertificationReport> load_report(std::string_view tenant_id,
                                                              std::string_view workflow_id) override;
private:
    static std::string key(std::string_view tenant_id, std::string_view workflow_id);
    std::mutex mutex_;
    std::map<std::string, StoredRoleCertificationCheckpoint> checkpoints_;
    std::map<std::string, StoredRoleCertificationReport> reports_;
};

struct SQLiteRoleCertificationStoreOptions { int busy_timeout_ms{3000}; bool require_private_permissions{true}; };
class SQLiteRoleCertificationStore final : public RoleCertificationStore {
public:
    explicit SQLiteRoleCertificationStore(std::string path,
        SQLiteRoleCertificationStoreOptions options = {});
    ~SQLiteRoleCertificationStore() override;
    SQLiteRoleCertificationStore(const SQLiteRoleCertificationStore&) = delete;
    SQLiteRoleCertificationStore& operator=(const SQLiteRoleCertificationStore&) = delete;
    RoleCertificationStoreCommit create(const RoleCertificationCheckpoint& value) override;
    std::optional<StoredRoleCertificationCheckpoint> load(std::string_view tenant_id,
                                                           std::string_view workflow_id) override;
    RoleCertificationStoreCommit compare_exchange(const RoleCertificationCheckpoint& value,
                                                   std::uint64_t expected_revision) override;
    RoleCertificationStoreCommit commit_report(const RoleCertificationCheckpoint& terminal,
                                                std::uint64_t expected_revision,
                                                const RoleCertificationReport& report) override;
    std::optional<StoredRoleCertificationReport> load_report(std::string_view tenant_id,
                                                              std::string_view workflow_id) override;
private:
    void migrate();
    void* db_{nullptr};
    std::string path_;
    SQLiteRoleCertificationStoreOptions options_;
    std::mutex mutex_;
};

struct LiveCellExecutionRequest {
    LiveEnvironmentProfile environment;
    RoleLiveMatrix matrix;
    LiveCellSpec cell;
    std::vector<LiveCellResult> prior_results;
};

class LiveCellExecutor {
public:
    virtual ~LiveCellExecutor() = default;
    virtual LiveCellResult execute(const LiveCellExecutionRequest& request) = 0;
};

struct RoleRuntimeCellBinding {
    llm_runtime::RoleInvocationRequest request;
    std::vector<std::string> evidence_digests;
    std::vector<std::string> oracle_digests;
    bool read_only{false};
    bool blind{false};
};

class RoleRuntimeLiveCellExecutor final : public LiveCellExecutor {
public:
    explicit RoleRuntimeLiveCellExecutor(std::shared_ptr<llm_runtime::RoleRuntime> runtime);
    bool bind(std::string cell_id, RoleRuntimeCellBinding binding);
    LiveCellResult execute(const LiveCellExecutionRequest& request) override;
private:
    std::shared_ptr<llm_runtime::RoleRuntime> runtime_;
    std::map<std::string, RoleRuntimeCellBinding> bindings_;
};

struct RoleCertificationOptions {
    std::string workflow_id;
    std::string now;
    std::string approval_decision_id;
    std::function<bool(std::string_view report_digest, std::string_view decision_id)> approval_validator;
    std::function<SignatureEnvelope(std::string_view signing_digest)> signer;
    std::function<bool(const SignatureEnvelope&)> signature_verifier;
    std::function<void(std::string_view severity, std::string_view message)> alert;
    std::function<bool()> cancelled;
};

struct RoleCertificationResult {
    RoleCertificationState state{RoleCertificationState::Failed};
    RoleCertificationCheckpoint checkpoint;
    std::optional<RoleCertificationReport> report;
    std::string error_code;
    std::string error_message;
};

class RoleLiveCertificationWorkflow {
public:
    RoleLiveCertificationWorkflow(RoleCertificationStore& store, LiveCellExecutor& executor);
    RoleCertificationResult run(const LiveEnvironmentProfile& environment,
                                const RoleLiveMatrix& matrix,
                                const RoleCertificationOptions& options);
private:
    RoleCertificationStore& store_;
    LiveCellExecutor& executor_;
};

std::vector<std::string> validate_live_contract(const LiveEnvironmentProfile& environment,
                                                const RoleLiveMatrix& matrix);
std::vector<std::string> validate_live_results(const LiveEnvironmentProfile& environment,
                                               const RoleLiveMatrix& matrix,
                                               const std::vector<LiveCellResult>& results);
bool role_certification_valid_for(const RoleCertificationReport& report,
                                  const LiveEnvironmentProfile& environment,
                                  const RoleLiveMatrix& matrix,
                                  std::string_view now,
                                  const std::function<bool(const SignatureEnvelope&)>& verifier);

}  // namespace agent_framework::live
