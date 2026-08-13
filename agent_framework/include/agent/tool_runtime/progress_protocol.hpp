#pragma once
#include "agent/tool_runtime/types.hpp"
#include "agent/conversation/types.hpp"
namespace agent_framework::tool_runtime {
enum class ProgressDisposition { AggregateEphemeral, PersistDurable, WakeOrchestrator };
ProgressDisposition classify_progress(const InvocationEvent&) noexcept;
std::optional<conversation::RuntimeEventEnvelope> project_runtime_event(
    const LongRunningToolInvocation&, const InvocationEvent&, std::string* error=nullptr);
}
