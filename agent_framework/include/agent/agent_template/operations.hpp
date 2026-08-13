#pragma once

#include "agent/agent_template/runtime.hpp"
#include "agent/ui/phase4_operations.hpp"

namespace agent_framework::agent_template {

class AgentTemplateOperationsProjection {
public:
    static void merge(Phase4OperationsSnapshot& snapshot, const AgentRunResult& result);
};

}  // namespace agent_framework::agent_template
