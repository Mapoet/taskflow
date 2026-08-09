#pragma once

#include <string>

#include "agent/run/types.hpp"

namespace agent_framework::run {

bool is_terminal(RunState state) noexcept;
bool can_transition(RunState from, RunState to) noexcept;
std::string run_state_name(RunState state);

}  // namespace agent_framework::run
