#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "agent/contracts/contract.hpp"

namespace agent_framework::run {

enum class RunState {
    Created, Received, Planning, AwaitingApproval, Running, Waiting, Interrupted,
    Verifying, Replanning, Completed, Partial, Rejected, Failed, Cancelled
};

struct RunCheckpoint {
    contracts::ContractMetadata metadata;
    std::uint64_t revision{1};
    RunState state{RunState::Received};
    std::string graph_revision;
    std::string node_id;
    std::uint64_t attempt{0};
    std::vector<std::string> pending_successors;
    std::string input_digest;
    std::string output_digest;
    std::string plan_digest;
    std::string acceptance_contract_digest;
    std::string effect_journal_position;
    std::string memory_snapshot_id;
    std::string memory_view_digest;
    std::string created_at;
};

struct Interruption {
    contracts::ContractMetadata metadata;
    std::string interruption_id;
    std::string kind;
    nlohmann::json payload = nlohmann::json::object();
    std::string policy_revision;
    std::string resume_token_digest;
    std::string state_digest;
    std::string expires_at;
};

nlohmann::json encode(const RunCheckpoint& value);
nlohmann::json encode(const Interruption& value);
std::optional<RunCheckpoint> decode_run_checkpoint(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<Interruption> decode_interruption(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);

}  // namespace agent_framework::run
