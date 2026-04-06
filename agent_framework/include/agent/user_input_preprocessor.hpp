/**
 * @file user_input_preprocessor.hpp
 * @brief WP2.7：Tier A/B 用户输入预处理（@file / @url / /cmd）
 * @author Mapoet
 * @version 0.1
 * @date 2026-04-05
 */
#ifndef __AGENT_USER_INPUT_PREPROCESSOR_H__
#define __AGENT_USER_INPUT_PREPROCESSOR_H__

#include <agent/execution_context.hpp>
#include <agent/types.hpp>
#include <agent/user_input_types.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace agent_framework {

class ToolBus;
class LLMClient;
struct AgentMessage;
struct AgentTask;
namespace internal {
struct AgentThreadState;
}

/** @brief true 除非 AGENT_INPUT_STRICT=0 */
bool env_input_strict_enabled();

/** @brief true 当 AGENT_INPUT_TIER_B=1 */
bool env_input_tier_b_enabled();

struct PreprocessOptions {
    bool enable_tier_b = false;
    std::shared_ptr<LLMClient> tier_b_llm;
    std::shared_ptr<ToolBus> toolbus;
    const AgentConfig* agent_config = nullptr;
};

/**
 * @brief A2A AgentMessage 中 TEXT part 按顺序拼接为 UTF-8 串
 */
std::string concat_user_text_from_message(const AgentMessage& msg);

/**
 * @brief 若 metadata 含 wp27_pending，回填 AgentThreadState 的 pending 字段
 */
void wp27_restore_pending_from_task_metadata(const AgentTask& task, internal::AgentThreadState& st);

/**
 * @brief 将 pending 注入与控制动作序列化进 task.metadata["wp27_pending"]
 */
void wp27_store_pending_in_task_metadata(const ProcessedUserInput& processed,
                                           const ExecutionContext& ctx,
                                           json& metadata_io);

class UserInputPreprocessor {
public:
    explicit UserInputPreprocessor(PreprocessOptions opt);

    ProcessedUserInput process(std::string_view raw_user_text, const ExecutionContext& ctx);

private:
    PreprocessOptions opt_;
};

/** @brief §3.4：按顺序拼入 LLMInput.context 正文并清空 blocks */
std::string take_injected_blocks_as_llm_context(std::vector<InjectedContextBlock>& blocks);

/** @brief 首轮 LLM 前分发 control_actions stub（WP2.9 实现体） */
void dispatch_pending_control_actions(std::vector<ControlAction>& actions,
                                      const ExecutionContext* ctx);

/**
 * @brief CLI/集成：将 process 结果写入 state（移动的语义）
 */
void apply_processed_to_agent_state(ProcessedUserInput&& processed, const ExecutionContext& ctx,
                                    internal::AgentThreadState& st);

} // namespace agent_framework

#endif // __AGENT_USER_INPUT_PREPROCESSOR_H__
