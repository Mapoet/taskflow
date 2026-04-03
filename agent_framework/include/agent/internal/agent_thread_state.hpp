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

#include <optional>
#include <string>
#include <vector>

namespace agent_framework {
namespace internal {

struct AgentThreadState {
    std::vector<Message> history;
    int iteration = 0;  // number of completed LLM calls
    std::string last_error;
    std::string initial_user_prompt;
    /** WP1.8：首轮 LLM 前解析；后续迭代复用，避免 tool 轮重选技能 */
    std::optional<std::string> skill_prompt_cache;
    std::optional<std::string> active_skill_id;
};

} // namespace internal
} // namespace agent_framework

#endif // __AGENT_INTERNAL_AGENT_THREAD_STATE_HPP__
