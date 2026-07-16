/**
 * @file llm_retry.hpp
 * @brief LLM HTTP 调用重试（指数退避）
 */
#ifndef __AGENT_LLM_RETRY_H__
#define __AGENT_LLM_RETRY_H__

#include <agent/core/types.hpp>
#include <functional>

namespace agent_framework {

/** 对可重试的 llm_http_error 做有限次重试后返回 LLMOutput */
LLMOutput invoke_with_retries(std::function<LLMOutput()> fn, const ModelConfig& config);

} // namespace agent_framework

#endif
