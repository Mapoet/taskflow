#include <agent/core/types.hpp>
#include <agent/agent/execution_context.hpp>
#include <agent/graph_executor/graph_executor.hpp>
#include <agent/skills/skill_runtime.hpp>
#include <agent/toolbus/toolbus.hpp>
#include <agent/ui/ui_manager.hpp>
#include <node/nodes.hpp>

// One-release source compatibility contract.
#include <agent/types.hpp>
#include <agent/execution_context.hpp>
#include <agent/graph_executor.hpp>
#include <agent/skill_runtime.hpp>
#include <agent/toolbus.hpp>
#include <agent/ui_manager.hpp>

#include <iostream>

int main() {
    agent_framework::ExecutionContext context;
    agent_framework::SkillRuntimeLimits limits;
    if(context.skill_max_input_bytes == 0 || limits.max_input_bytes == 0) return 1;
    std::cout << "canonical-and-legacy-public-headers-ok\n";
    return 0;
}
