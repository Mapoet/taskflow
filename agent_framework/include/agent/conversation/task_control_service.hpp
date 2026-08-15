#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "agent/conversation/task_registry.hpp"
#include "agent/tool_runtime/execution_control.hpp"
#include "agent/tool_runtime/incremental_result_store.hpp"
#include "agent/tool_runtime/store.hpp"
namespace agent_framework::conversation {
struct TaskControlResult { bool ok{false}; std::size_t affected{0}; std::vector<std::string> errors; };
class TaskControlService {
public:
 TaskControlService(TaskRegistry&,tool_runtime::InvocationStore&,tool_runtime::ExecutionControlStore&,tool_runtime::IncrementalResultViewAssembler* = nullptr);
 nlohmann::json status(const ConversationIdentity&,std::string_view task_id);
 TaskControlResult cancel(const ConversationIdentity&,std::string_view task_id,std::string reason,std::int64_t now_ms);
private:
 TaskRegistry& tasks_; tool_runtime::InvocationStore& invocations_;
 tool_runtime::ExecutionControlStore& controls_; tool_runtime::IncrementalResultViewAssembler* results_;
};
}
