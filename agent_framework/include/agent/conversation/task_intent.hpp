#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/conversation/types.hpp"

namespace agent_framework::conversation {

enum class TaskIntentKind { NewTask, Continue, AddRequirement, NarrowScope, Replan,
                            Pause, Cancel, StatusQuery, ProfileConfirmation };
enum class EffectClass { None, ReadOnly, WorkspaceWrite, External, Destructive };

std::string_view name(TaskIntentKind value);
std::optional<TaskIntentKind> task_intent_kind(std::string_view value);
std::string_view name(EffectClass value);

struct LinguisticEvidence {
    std::vector<std::string> requested_actions, negated_actions, mention_only,
        scope_constraints, ambiguities;
};

struct TaskIntentDecision {
    TaskIntentKind intent{TaskIntentKind::NewTask};
    TaskExecutionProfile suggested_profile{TaskExecutionProfile::Conversation};
    EffectClass effect_class{EffectClass::None};
    bool long_running{false};
    double confidence{0.0};
    LinguisticEvidence evidence;
    // Semantic routing never grants authority; PDP/Approval/Tool policy own it.
    bool grants_authority{false};
};

} // namespace agent_framework::conversation
