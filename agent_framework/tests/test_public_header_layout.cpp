#include <agent/core/types.hpp>
#include <agent/agent/execution_context.hpp>
#include <agent/graph_executor/graph_executor.hpp>
#include <agent/skills/skill_runtime.hpp>
#include <agent/toolbus/toolbus.hpp>
#include <agent/ui/ui_manager.hpp>
#include <node/nodes.hpp>

#include <iostream>

int main() {
    agent_framework::ExecutionContext context;
    agent_framework::SkillRuntimeLimits limits;
    if(context.skill_max_input_bytes == 0 || limits.max_input_bytes == 0) return 1;
    std::cout << "canonical-public-headers-ok\n";
    return 0;
}
