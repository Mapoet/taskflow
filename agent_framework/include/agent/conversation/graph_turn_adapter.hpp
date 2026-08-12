#pragma once
#include "agent/conversation/types.hpp"
#include "agent/graph_executor/graph_executor.hpp"
namespace agent_framework::conversation {
class GraphTurnAdapter {
public:
    static ModelTurnOutcome from_execution(const ExecutionResult&);
    static ModelTurnOutcome from_workflow(const WorkflowResult&);
};
}
