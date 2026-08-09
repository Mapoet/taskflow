/**
 * @file agent_thread_state.hpp
 * @brief WP1.5 agent loop thread state (internal)
 * @author Mapoet
 * @version 0.1
 * @date 2026-03-31
 */
#ifndef __AGENT_INTERNAL_AGENT_THREAD_STATE_HPP__
#define __AGENT_INTERNAL_AGENT_THREAD_STATE_HPP__

#include <agent/core/types.hpp>
#include <agent/agent/execution_context.hpp>
#include <agent/agent/user_input_types.hpp>

#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace agent_framework {
namespace a2a {
class OutboundTaskSupervisor;
}

namespace internal {

struct AgentThreadState {
    std::vector<Message> history;
    int iteration = 0;  // number of completed LLM calls
    std::string last_error;
    std::string initial_user_prompt;
    /** WP1.8：首轮 LLM 前解析；后续迭代复用，避免 tool 轮重选技能 */
    std::optional<std::string> skill_prompt_cache;
    std::optional<std::string> active_skill_id;
    /** WP2.agents：可选；与 A2A 细粒度工具共用 */
    std::shared_ptr<a2a::OutboundTaskSupervisor> outbound_supervisor;

    /** WP2.7：首轮 LLM 前消费；由 UserInputPreprocessor / A2A metadata 填入 */
    std::optional<ExecutionContext> execution_context;
    std::vector<InjectedContextBlock> pending_injected_context;
    std::vector<ControlAction> pending_control_actions;
    std::vector<std::string> pending_input_violations;

    /** WP2.8：Verifier 触发的 MAIN 额外次数（FIX 回到 MAIN 前递增） */
    int verifier_retry_count = 0;

    /** WP2.9：自动压缩节流；`-1` 表示尚未压缩 */
    int last_memory_auto_compact_iteration = -1;
    std::optional<std::time_t> last_memory_compaction_ts;
    /** Exact last result and per-execution reports; never contain source conversation text. */
    json last_memory_compaction_report = json::object();
    std::vector<json> memory_compaction_reports;

    /** Last prompt assembly telemetry; contains only counts/digests, never context text. */
    json last_memory_assembly_report = json::object();
    /** Per-iteration telemetry for the current execution; GraphExecutor drains it to events. */
    std::vector<json> memory_assembly_reports;
};

} // namespace internal
} // namespace agent_framework

#endif // __AGENT_INTERNAL_AGENT_THREAD_STATE_HPP__
