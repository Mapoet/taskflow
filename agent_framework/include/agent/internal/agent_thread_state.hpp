/**
 * @file agent_thread_state.hpp
 * @brief WP1.5 agent loop thread state (internal)
 * @author Mapoet
 * @version 0.1
 * @date 2026-03-31
 */
#ifndef __AGENT_INTERNAL_AGENT_THREAD_STATE_HPP__
#define __AGENT_INTERNAL_AGENT_THREAD_STATE_HPP__

#include "agent/types.hpp"

#include <string>
#include <vector>

namespace agent_framework {
namespace internal {

struct AgentThreadState {
    std::vector<Message> history;
    int iteration = 0;  // number of completed LLM calls
    std::string last_error;
    std::string initial_user_prompt;
};

} // namespace internal
} // namespace agent_framework

#endif // __AGENT_INTERNAL_AGENT_THREAD_STATE_HPP__
