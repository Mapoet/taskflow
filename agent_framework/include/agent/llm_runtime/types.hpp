#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/contracts/contract.hpp"
#include "agent/core/types.hpp"

namespace agent_framework::llm_runtime {

enum class ReasoningEffort { Minimal, Low, Medium, High };
enum class EvidenceAuthority { Candidate, Derived, Advisory };
enum class InvocationState { Pending, Running, Succeeded, Failed, Cancelled, ManualReview };
enum class FailureClass { None, Retryable, NonRetryable, PolicyDenied, OutputInvalid, Cancelled };

struct ProviderModel {
    std::string candidate_id;
    std::string provider;
    std::string model;
    std::string adapter_revision;
    std::vector<std::string> capabilities;
    std::vector<std::string> regions;
    std::string model_family;
    std::string independence_group;
    std::uint64_t max_context_tokens{0};
    std::optional<double> input_cost_per_million;
    std::optional<double> output_cost_per_million;
    bool available{true};
    int priority{100};
};

struct LLMRoleProfile {
    contracts::ContractMetadata metadata;
    std::string profile_id;
    std::string revision;
    std::string role;
    std::vector<std::string> provider_pool;
    ReasoningEffort reasoning_effort{ReasoningEffort::Medium};
    double temperature{0.0};
    double top_p{1.0};
    std::uint64_t max_context_tokens{0};
    int max_output_tokens{4096};
    std::string prompt_id;
    std::string prompt_revision;
    std::string memory_view_profile;
    std::vector<std::string> required_capabilities;
    std::vector<std::string> allowed_regions;
    std::string independence_group;
    EvidenceAuthority evidence_authority{EvidenceAuthority::Candidate};
    int timeout_ms{120000};
    int max_attempts{2};
    int max_fallbacks{1};
    std::optional<double> max_cost_usd;
    std::string calibration_revision;
    bool enabled{true};
    std::map<std::string, nlohmann::json> provider_parameters;
};

struct PromptRevision {
    contracts::ContractMetadata metadata;
    std::string prompt_id;
    std::string revision;
    std::string system_template;
    std::string user_template;
    nlohmann::json input_schema = nlohmann::json::object();
    nlohmann::json output_schema = nlohmann::json::object();
    bool structured_output_required{true};
    int max_repair_attempts{1};
    std::vector<contracts::RedactionRule> redaction_rules;
    std::string compatibility_class;
    bool deprecated{false};
};

struct ModelRouteDecision {
    contracts::ContractMetadata metadata;
    std::string route_id;
    std::string profile_id;
    std::string profile_revision;
    bool selected{false};
    std::string candidate_id;
    std::string provider;
    std::string model;
    std::string adapter_revision;
    std::string decision_code;
    std::string decision_message;
    std::vector<std::string> rejected_candidates;
    std::optional<double> estimated_cost_usd;
    std::string policy_revision;
};

struct UsageRecord {
    std::optional<std::uint64_t> input_tokens;
    std::optional<std::uint64_t> output_tokens;
    std::optional<std::uint64_t> cached_input_tokens;
    std::optional<double> cost_usd;
    std::string source;
    std::string unknown_reason;
};

struct InvocationAttempt {
    std::uint64_t sequence{0};
    std::string candidate_id;
    std::string provider;
    std::string model;
    std::string started_at;
    std::string finished_at;
    FailureClass failure_class{FailureClass::None};
    std::string error_code;
    std::string error_message;
    bool fallback{false};
    bool output_repair{false};
    UsageRecord usage;
};

struct LLMInvocationManifest {
    contracts::ContractMetadata metadata;
    std::string invocation_id;
    InvocationState state{InvocationState::Pending};
    std::string role;
    std::string profile_id;
    std::string profile_revision;
    std::string prompt_id;
    std::string prompt_revision;
    std::string prompt_digest;
    std::string route_decision_digest;
    std::string candidate_id;
    std::string provider;
    std::string model;
    std::string adapter_revision;
    std::string reasoning_effort;
    std::string independence_group;
    std::string evidence_authority;
    std::string calibration_revision;
    std::string memory_snapshot_id;
    std::string memory_view_profile;
    std::string memory_view_digest;
    std::vector<std::string> capabilities;
    std::string capability_digest;
    std::string input_digest;
    std::string output_digest;
    std::vector<InvocationAttempt> attempts;
    UsageRecord usage;
    std::uint64_t latency_ms{0};
    std::string started_at;
    std::string finished_at;
    std::string error_code;
    std::string error_message;
};

struct ReasoningArtifact {
    contracts::ContractMetadata metadata;
    std::string artifact_id;
    std::vector<std::string> claims;
    std::vector<std::string> evidence_ids;
    std::vector<std::string> assumptions;
    std::vector<std::string> unknowns;
    std::vector<std::string> alternatives;
    std::vector<std::string> decision_rationale;
    std::vector<std::string> risks;
    std::vector<std::string> counterexamples;
    double confidence{0.0};
};

struct RoleCalibrationRecord {
    contracts::ContractMetadata metadata;
    std::string calibration_id;
    std::string role;
    std::string profile_id;
    std::string profile_revision;
    std::string prompt_revision;
    std::string provider;
    std::string model;
    std::string dataset_revision;
    std::map<std::string, double> metrics;
    std::map<std::string, double> thresholds;
    bool approved{false};
    std::string decision_id;
};

struct MemoryViewBinding {
    std::string snapshot_id;
    std::string profile;
    std::string view_digest;
};

struct IndependenceRequirement {
    std::vector<std::string> forbidden_groups;
    std::vector<std::string> forbidden_providers;
    std::vector<std::string> forbidden_models;
    bool require_provider_diversity{false};
    bool require_model_diversity{false};
};

struct RoleInvocationRequest {
    contracts::ContractMetadata metadata;
    std::string invocation_id;
    std::string trace_id;
    std::string parent_span_id;
    std::string profile_id;
    std::string profile_revision;
    std::map<std::string, std::string> prompt_variables;
    LLMInput input;
    MemoryViewBinding memory_view;
    std::vector<std::string> granted_capabilities;
    IndependenceRequirement independence;
    std::string required_region;
    std::uint64_t estimated_input_tokens{0};
    std::string policy_revision;
};

struct RoleInvocationResult {
    bool ok{false};
    LLMOutput output;
    std::optional<nlohmann::json> structured_output;
    LLMInvocationManifest manifest;
    std::string error_code;
    std::string error_message;
};

std::string reasoning_effort_name(ReasoningEffort value);
std::optional<ReasoningEffort> reasoning_effort_from_name(const std::string& value);
std::string evidence_authority_name(EvidenceAuthority value);
std::optional<EvidenceAuthority> evidence_authority_from_name(const std::string& value);
std::string invocation_state_name(InvocationState value);
std::optional<InvocationState> invocation_state_from_name(const std::string& value);
std::string failure_class_name(FailureClass value);
std::optional<FailureClass> failure_class_from_name(const std::string& value);

nlohmann::json encode(const LLMRoleProfile& value);
nlohmann::json encode(const PromptRevision& value);
nlohmann::json encode(const ModelRouteDecision& value);
nlohmann::json encode(const LLMInvocationManifest& value);
nlohmann::json encode(const ReasoningArtifact& value);
nlohmann::json encode(const RoleCalibrationRecord& value);

std::optional<LLMRoleProfile> decode_role_profile(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<PromptRevision> decode_prompt_revision(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<ModelRouteDecision> decode_route_decision(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<LLMInvocationManifest> decode_invocation_manifest(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<ReasoningArtifact> decode_reasoning_artifact(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<RoleCalibrationRecord> decode_calibration_record(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);

std::vector<contracts::ContractIssue> validate(const LLMRoleProfile& value);
std::vector<contracts::ContractIssue> validate(const PromptRevision& value);
std::vector<contracts::ContractIssue> validate(const LLMInvocationManifest& value);

}  // namespace agent_framework::llm_runtime
