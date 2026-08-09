#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "agent/contracts/contract.hpp"

namespace agent_framework::planning {

struct TaskIntake {
    contracts::ContractMetadata metadata;
    std::string user_goal;
    std::vector<std::string> requested_deliverables;
    std::vector<std::string> explicit_constraints;
    std::vector<std::string> granted_authorities;
    std::vector<std::string> success_signals;
    std::vector<std::string> fact_gaps;
};

struct EvidenceRecord {
    std::string evidence_id;
    std::string origin_kind;
    std::string locator;
    std::string content_digest;
    std::string collected_at;
    std::string trust_class;
    std::string freshness_deadline;
    std::vector<std::string> supported_claims;
    std::vector<std::string> contradicted_claims;
    bool instruction_authority{false};
};

struct EvidenceBundle {
    contracts::ContractMetadata metadata;
    std::string bundle_id;
    std::vector<EvidenceRecord> records;
};

enum class ChangeMode { Repair, Incremental, Refactor, Upgrade, Transformation };

struct TaskUnderstanding {
    contracts::ContractMetadata metadata;
    std::string domain;
    std::string current_state;
    std::string target_state;
    std::vector<std::string> gaps;
    std::vector<std::string> assumptions;
    std::vector<std::string> unknowns;
    std::vector<std::string> risks;
    std::vector<std::string> evidence_ids;
    ChangeMode change_mode{ChangeMode::Incremental};
    std::string blast_radius;
};

struct ResourceBudget {
    std::uint64_t wall_time_ms{0};
    std::uint64_t token_budget{0};
    std::uint64_t tool_calls{0};
    double cost_limit{0.0};
};

struct PlanNode {
    std::string node_id;
    std::string objective;
    std::vector<std::string> in_scope;
    std::vector<std::string> out_of_scope;
    std::vector<std::string> input_contracts;
    std::vector<std::string> output_contracts;
    std::vector<std::string> dependencies;
    std::vector<std::string> required_capabilities;
    std::vector<std::string> side_effects;
    std::string acceptance_contract_id;
    std::string rollback_strategy;
    std::string risk_level;
    bool approval_required{false};
};

struct ExecutionPlan {
    contracts::ContractMetadata metadata;
    std::uint64_t plan_revision{1};
    std::string parent_plan_digest;
    std::string task_understanding_digest;
    std::string evidence_bundle_digest;
    std::string acceptance_contract_digest;
    std::string memory_snapshot_id;
    std::string planning_view_digest;
    std::vector<PlanNode> nodes;
    std::vector<std::string> critical_path;
    ResourceBudget budget;
};

std::string to_string(ChangeMode mode);
std::optional<ChangeMode> change_mode_from_string(const std::string& value);

nlohmann::json encode(const TaskIntake& value);
nlohmann::json encode(const EvidenceBundle& value);
nlohmann::json encode(const TaskUnderstanding& value);
nlohmann::json encode(const ExecutionPlan& value);

std::optional<TaskIntake> decode_task_intake(const nlohmann::json& value,
                                             const contracts::ParseContext& context = {},
                                             std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<EvidenceBundle> decode_evidence_bundle(const nlohmann::json& value,
                                                     const contracts::ParseContext& context = {},
                                                     std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<TaskUnderstanding> decode_task_understanding(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<ExecutionPlan> decode_execution_plan(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);

}  // namespace agent_framework::planning
