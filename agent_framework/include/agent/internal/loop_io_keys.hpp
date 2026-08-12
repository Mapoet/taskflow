/**
 * @file loop_io_keys.hpp
 * @brief WP1.5 loop I/O keys constants (internal)
 * @author Mapoet
 * @version 0.1
 * @date 2026-03-31
 */
#ifndef __AGENT_INTERNAL_LOOP_IO_KEYS_HPP__
#define __AGENT_INTERNAL_LOOP_IO_KEYS_HPP__

#include <string_view>

namespace agent_framework {
namespace internal {

// Inputs (outer graph -> loop body)
constexpr std::string_view kUserQuery = "query";
constexpr std::string_view kSystemPrompt = "prompt";
constexpr std::string_view kAgentState = "agent_state";

// LLM outputs
constexpr std::string_view kLlmOutput = "llm_output";

// Tool execution outputs
constexpr std::string_view kToolMessages = "tool_messages";
constexpr std::string_view kToolHadError = "tool_had_error";

// State merge outputs
constexpr std::string_view kNextAgentState = "next_agent_state";
constexpr std::string_view kIsFinal = "is_final";
constexpr std::string_view kFinalAnswer = "final_answer";
// `is_final` is retained for loop compatibility and means only that the model turn stopped.
// It is never task-completion authority; production completion is issued by TaskClosureController.
constexpr std::string_view kModelStopReason = "model_stop_reason";

} // namespace internal
} // namespace agent_framework

#endif // __AGENT_INTERNAL_LOOP_IO_KEYS_HPP__
