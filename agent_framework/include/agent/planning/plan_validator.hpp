#pragma once

#include <string>
#include <vector>

#include "agent/planning/types.hpp"

namespace agent_framework::planning {

enum class PlanIssueSeverity { Warning, Error };
struct PlanIssue {
    PlanIssueSeverity severity{PlanIssueSeverity::Error};
    std::string code;
    std::string node_id;
    std::string message;
};
struct PlanValidationResult {
    std::vector<PlanIssue> issues;
    bool valid() const noexcept;
};

class PlanValidator {
public:
    PlanValidationResult validate(const ExecutionPlan& plan) const;
};

}  // namespace agent_framework::planning
