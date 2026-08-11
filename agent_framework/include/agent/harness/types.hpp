#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/contracts/contract.hpp"

namespace agent_framework::harness {

enum class HarnessStage {
    Intake,
    Cognition,
    PlanApproval,
    Execution,
    MemoryUpdate,
    Assurance,
    Remediation,
    Reexecution,
    Reverification,
    Judge,
    Operations,
    Complete
};

enum class HarnessState {
    Running,
    AwaitingApproval,
    ManualReview,
    Completed,
    Rejected,
    Failed,
    Cancelled
};

enum class StageOutcome {
    Succeeded,
    AwaitingApproval,
    NeedsRemediation,
    Rejected,
    Retryable,
    ManualReview,
    Failed,
    Cancelled
};

enum class OutboxState { Pending, Committed, Rejected, Unknown };

struct PinnedRevisions {
    std::string intake_digest;
    std::string plan_digest;
    std::string acceptance_contract_digest;
    std::string memory_snapshot_id;
    std::string memory_view_digest;
    std::string profile_revision_digest;
    std::string prompt_revision_digest;
    std::string approval_decision_id;
    std::string artifact_manifest_digest;
    std::string acceptance_report_digest;
    std::string judge_report_digest;
    std::string operations_snapshot_digest;
};

struct HarnessOutboxEntry {
    std::string effect_id;
    HarnessStage stage{HarnessStage::Intake};
    std::uint64_t attempt{0};
    std::string idempotency_key;
    std::string request_digest;
    OutboxState state{OutboxState::Pending};
    std::string receipt_digest;
    std::string error;
};

struct HarnessStageRecord {
    HarnessStage stage{HarnessStage::Intake};
    std::uint64_t attempt{0};
    StageOutcome outcome{StageOutcome::Failed};
    std::string effect_id;
    std::string invocation_manifest_digest;
    std::string output_digest;
    std::vector<std::string> finding_ids;
    std::string error_code;
    std::string error_message;
};

struct HarnessCheckpoint {
    contracts::ContractMetadata metadata;
    std::string harness_id;
    std::uint64_t revision{1};
    HarnessState state{HarnessState::Running};
    HarnessStage next_stage{HarnessStage::Intake};
    std::uint64_t remediation_cycle{0};
    std::uint64_t max_remediation_cycles{2};
    bool judge_required{true};
    PinnedRevisions pins;
    std::vector<HarnessStageRecord> stage_records;
    std::vector<HarnessOutboxEntry> outbox;
    std::vector<std::string> unresolved_findings;
    std::string terminal_reason;
    std::string updated_at;
};

std::string harness_stage_name(HarnessStage value);
std::optional<HarnessStage> harness_stage_from_name(std::string_view value);
std::string harness_state_name(HarnessState value);
std::optional<HarnessState> harness_state_from_name(std::string_view value);
std::string stage_outcome_name(StageOutcome value);
std::optional<StageOutcome> stage_outcome_from_name(std::string_view value);
std::string outbox_state_name(OutboxState value);
std::optional<OutboxState> outbox_state_from_name(std::string_view value);

nlohmann::json encode(const HarnessCheckpoint& value);
std::optional<HarnessCheckpoint> decode_harness_checkpoint(
    const nlohmann::json& value,
    const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);

}  // namespace agent_framework::harness
