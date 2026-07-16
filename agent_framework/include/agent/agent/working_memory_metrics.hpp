/**
 * @file working_memory_metrics.hpp
 * @brief WP2.9：工作记忆计量 JSON 导出（phase-2-wp9.md §2）
 */
#ifndef __AGENT_WORKING_MEMORY_METRICS_H__
#define __AGENT_WORKING_MEMORY_METRICS_H__

#include <agent/core/types.hpp>

#include <cstddef>

namespace agent_framework {
namespace internal {
struct AgentThreadState;
}

/**
 * @brief UTF-8 字节计量（tool 用 tool_result 序列化，否则 content）
 */
std::size_t history_message_utf8_bytes(const Message& m);

/** @brief 所有 history 消息字节之和 */
std::size_t history_utf8_bytes_total(const internal::AgentThreadState& st);

/**
 * @brief wp9 §2.2 固定键 JSON（含阈值与触发指示）
 */
json working_memory_metrics(const internal::AgentThreadState& st);

} // namespace agent_framework

#endif // __AGENT_WORKING_MEMORY_METRICS_H__
