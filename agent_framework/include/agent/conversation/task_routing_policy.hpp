#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "agent/conversation/task_classifier.hpp"

namespace agent_framework::conversation {

enum class TaskPromotionMode { DirectTurn, BoundedTask, LongRunningTask, ContinuousTask };
enum class PlanningDepth { None, Bounded, Comprehensive, Continuous };

std::string_view name(TaskPromotionMode);
std::string_view name(PlanningDepth);

struct TaskRoutingDecision {
    TaskPromotionMode promotion{TaskPromotionMode::DirectTurn};
    PlanningDepth planning_depth{PlanningDepth::None};
    bool promote_to_task{false};
    bool planning_required{false};
    std::vector<std::string> reasons;
    std::string policy_revision{"task-promotion-planning-v1"};
};

// Pure, deterministic policy. It interprets semantic evidence but grants no
// execution authority; effect policy/PDP/approval remain separate controls.
TaskRoutingDecision decide_task_route(const TaskClassification&,
                                      bool user_requested_plan=false,
                                      std::size_t known_dependency_count=0);

}  // namespace agent_framework::conversation
