/**
 * @file memory_compaction.hpp
 * @brief WP2.9：run_memory_compaction 唯一策略入口与自动触发（phase-2-wp9.md）
 */
#ifndef __AGENT_MEMORY_COMPACTION_H__
#define __AGENT_MEMORY_COMPACTION_H__

#include <agent/core/types.hpp>
#include <agent/agent/working_memory_metrics.hpp>

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

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

enum class MemoryCompactionOutcome {
    Mutated,
    NoOp,
    Failed,
    Cancelled
};

struct MemoryCompactResult {
    MemoryCompactionOutcome outcome = MemoryCompactionOutcome::NoOp;
    bool did_mutate = false;
    std::string strategy_used;  // "truncate" | "summarize" | "fallback_truncate"
    std::size_t bytes_before = 0;
    std::size_t bytes_after = 0;
    std::string log_reason;
    std::string source_digest;
    int schema_version = 1;
    bool cancelled = false;
};

struct MemoryCompactionProfile {
    std::string provider;
    int timeout_ms = 0;
    std::size_t max_input_bytes = 0;
    std::size_t max_output_bytes = 0;
    int max_retries = 0;
    double temperature = 0.0;
};

struct MemoryCompactionInput {
    internal::AgentThreadState& state;
    MemoryCompactTrigger trigger;
    std::size_t head_keep;
    std::size_t tail_keep;
    std::size_t bytes_before;
};

/** Deliberately contains no ToolBus or SkillServices capability. */
struct MemoryCompactionContext {
    const AgentConfig* agent_config = nullptr;
    LLMClient* sub_llm = nullptr;
    MemoryCompactionProfile profile;
    std::function<bool()> cancellation_requested;
};

class MemoryCompactor {
public:
    virtual ~MemoryCompactor() = default;
    virtual MemoryCompactResult compact(const MemoryCompactionInput& input,
                                        const MemoryCompactionContext& context) = 0;
};

class MemoryCompactorRegistry {
public:
    void register_compactor(std::string id, std::shared_ptr<MemoryCompactor> compactor);
    std::shared_ptr<MemoryCompactor> resolve(std::string_view id) const;
    std::vector<std::string> ids() const;
private:
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<MemoryCompactor>> compactors_;
};

std::shared_ptr<MemoryCompactorRegistry> default_memory_compactor_registry();

struct MemoryCompactOptions {
    const AgentConfig* agent_config = nullptr;
    /** Deprecated compatibility path for direct callers; AgentLoop uses sub_llm_client. */
    LLMClient* llm_client = nullptr;
    LLMClient* sub_llm_client = nullptr;
    std::shared_ptr<MemoryCompactorRegistry> registry;
    MemoryCompactionProfile profile;
    std::function<bool()> cancellation_requested;
    std::string strategy_id;
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
