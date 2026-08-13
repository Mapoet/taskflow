#pragma once
#include "agent/tool_runtime/types.hpp"
namespace agent_framework::tool_runtime
{
    bool can_transition(InvocationState from, InvocationState to) noexcept;
}
