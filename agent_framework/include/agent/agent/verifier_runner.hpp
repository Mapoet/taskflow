/**
 * @file verifier_runner.hpp
 * @brief WP2.8 Verifier LLM runner (phase-2-wp8.md §7)
 * @author Mapoet
 * @version 0.1
 * @date 2026-04-05
 */
#ifndef __AGENT_VERIFIER_RUNNER_H__
#define __AGENT_VERIFIER_RUNNER_H__

#include <agent/llm_client/llm_client.hpp>
#include <agent/core/types.hpp>
#include <agent/agent/verifier_types.hpp>

#include <chrono>
#include <memory>
#include <string>

namespace agent_framework {
namespace internal {
struct AgentThreadState;
}

/** @brief English system prompt: JSON only, no tools (wp8 §7). */
const char* verifier_system_prompt();

/**
 * @brief Second LLMClient profile: AGENT_VERIFIER_* overrides, else AGENT_LLM_* (wp8 §7).
 * Sets PromptRenderer; temperature default 0 unless AGENT_VERIFIER_TEMPERATURE set.
 */
std::shared_ptr<LLMClient> make_verifier_llm_client_from_env();

/**
 * @brief Build Verifier user message JSON (wp8 §3) with byte budget and draft tail truncation.
 * @param max_bytes AGENT_VERIFIER_PROMPT_MAX_BYTES (default 65536)
 */
std::string build_verifier_user_json(const internal::AgentThreadState& state,
                                    std::string_view draft_final_answer,
                                    std::size_t max_bytes);

struct VerifierRunOutput {
    VerifierResult parsed;
    int latency_ms = 0;
};

/**
 * @brief Invoke Verifier LLM, optional wall-clock timeout; parse with §4 defaults (wp8 §6.3).
 */
VerifierRunOutput run_verifier_sync(LLMClient& llm,
                                   std::string_view verifier_user_json,
                                   int verifier_retry_count,
                                   int max_retries,
                                   int timeout_ms);

std::size_t verifier_prompt_max_bytes_from_env();
int verifier_timeout_ms_from_env();
int verifier_max_retries_from_env();
bool verifier_include_tool_trace_from_env();
bool verifier_redact_ids_from_env();
int verifier_history_turns_from_env();

} // namespace agent_framework

#endif // __AGENT_VERIFIER_RUNNER_H__
