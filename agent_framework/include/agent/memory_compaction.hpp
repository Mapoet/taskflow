/**
 * @file memory_compaction.hpp
 * @brief WP2.9：run_memory_compaction 唯一策略入口与自动触发（phase-2-wp9.md）
 */
#ifndef __AGENT_MEMORY_COMPACTION_H__
#define __AGENT_MEMORY_COMPACTION_H__

#include <agent/types.hpp>
#include <agent/working_memory_metrics.hpp>

#include <cstddef>
#include <string>

namespace agent_framework {

class LLMClient;
struct AgentConfig;
namespace internal {
struct AgentThreadState;
}

enum class MemoryCompactTrigger {
    auto_threshold,
    manual_compact,
    hard_cap
};

struct MemoryCompactResult {
    bool did_mutate = false;
    std::string strategy_used;  // "truncate" | "summarize" | "fallback_truncate"
    std::size_t bytes_before = 0;
    std::size_t bytes_after = 0;
    std::string log_reason;
};

struct MemoryCompactOptions {
    const AgentConfig* agent_config = nullptr;
    LLMClient* llm_client = nullptr;
};

/**
 * @brief 唯一压缩入口：truncate / summarize（失败回退 truncate）+ §5.3 硬上限 1c 裁剪
 */
MemoryCompactResult run_memory_compaction(internal::AgentThreadState& st,
                                         MemoryCompactTrigger why,
                                         const MemoryCompactOptions& opt);

/**
 * @brief 每轮 Agent 迭代结束后：按 env 阈值与节流尝试自动压缩
 */
void maybe_auto_compact_memory(internal::AgentThreadState& st, const MemoryCompactOptions& opt);

/**
 * @brief WP2.9：清空工作记忆（与 /memory clear 一致）
 */
void apply_memory_clear(internal::AgentThreadState& st);

} // namespace agent_framework

#endif // __AGENT_MEMORY_COMPACTION_H__
