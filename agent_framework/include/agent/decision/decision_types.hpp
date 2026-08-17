#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/identity/runtime_subject.hpp"

namespace agent_framework::decision {

enum class DecisionKind { TaskSemantics, Planning, MemoryConflict, RunRouting, Recovery };
enum class DecisionState { Pending, Answered, Expired, Cancelled };

std::string_view name(DecisionKind);
std::optional<DecisionKind> decision_kind(std::string_view);
std::string_view name(DecisionState);
std::optional<DecisionState> decision_state(std::string_view);

struct DecisionOption {
    std::string option_id;
    std::string label;
    std::string description;
    nlohmann::json semantic_patch=nlohmann::json::object();
};

struct DecisionRequest {
    identity::RuntimeSubject subject;
    std::string decision_id;
    DecisionKind kind{DecisionKind::TaskSemantics};
    nlohmann::json resume_payload=nlohmann::json::object();
    std::string question;
    std::vector<DecisionOption> options;
    std::string recommended_option_id;
    std::string selected_option_id;
    std::uint64_t revision{1};
    std::uint64_t expires_at_ms{0};
    DecisionState state{DecisionState::Pending};
    std::string origin_digest;
    std::string created_at;
    std::string updated_at;
};

struct DecisionMutationResult {
    bool ok{false};
    std::uint64_t revision{0};
    DecisionState state{DecisionState::Pending};
    std::string error;
};

}  // namespace agent_framework::decision
