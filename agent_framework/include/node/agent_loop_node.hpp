/**
 * @file agent_loop_node.hpp
 * @brief Agent 循环节点封装：将完整的 Agent 循环封装为 workflow 循环节点
 * @author Mapoet
 * @version 0.1
 * @date 2025-01-XX
 */
#ifndef __AGENT_NODE_AGENT_LOOP_NODE_H__
#define __AGENT_NODE_AGENT_LOOP_NODE_H__

#include <workflow/nodeflow.hpp>
#include "../agent/types.hpp"
#include "../agent/llm_client.hpp"
#include "../agent/toolbus.hpp"
#include "../agent/skill_runtime.hpp"
#include "../agent/memory.hpp"
#include "../agent/vectorstore.hpp"
#include <functional>
#include <string>
#include <memory>
#include <string_view>

namespace agent_framework {

class TaskControl;

struct SkillServices;

namespace node {

/**
 * @brief Agent 循环节点封装类
 * 使用 create_loop_decl 将完整的 Agent Plan->Act->Observe->Reflect 循环封装为节点
 */
class AgentLoopNode {
public:
    /**
     * @brief 创建 Agent 循环节点
     * @param builder 图构建器
     * @param name 节点名称
     * @param agent_config Agent 配置
     * @param llm_client LLM 客户端
     * @param toolbus ToolBus 实例
     * @param memory_store 记忆存储
     * @param vector_store 向量存储（可选）
     * @param input_specs 输入规格：
     *   - {"UserInput", "query"} - 用户输入
     *   - {"SystemPrompt", "prompt"} - 系统提示词（可选）
     * @param output_keys 输出键列表：
     *   - "final_answer": 最终回答
     *   - "reasoning": 思考过程
     *   - "tool_calls": 工具调用列表
     * @param stream_callback 可选；传给 `LLMClient::invoke` 的流式 token 回调（WP1.6）
     * @param skills 可选 WP1.8；`nullptr` 关闭技能路由与 `skill_block` 注入
     * @param task_control 可选 WP2.3；协作式取消与 deadline 检查（AgentServer 注入）
     * @return (节点指针, 任务句柄)
     */
    static std::pair<std::shared_ptr<workflow::LoopNode>, tf::Task>
    create(
        workflow::GraphBuilder& builder,
        const std::string& name,
        const AgentConfig& agent_config,
        std::shared_ptr<LLMClient> llm_client,
        std::shared_ptr<ToolBus> toolbus,
        std::shared_ptr<MemoryStore> memory_store,
        std::shared_ptr<VectorStore> vector_store,
        const std::vector<std::pair<std::string, std::string>>& input_specs,
        const std::vector<std::string>& output_keys,
        std::function<void(std::string_view)> stream_callback = nullptr,
        std::shared_ptr<SkillServices> skills = nullptr,
        std::shared_ptr<TaskControl> task_control = nullptr,
        ToolExecutionObserver tool_execution_observer = {},
        SkillEventSink skill_event_sink = {}
    );

private:
    /**
     * @brief 构建循环体（每次迭代执行的子图）
     * @param builder 子图构建器
     * @param agent_config Agent 配置
     * @param llm_client LLM 客户端
     * @param toolbus ToolBus 实例
     * @param memory_store 记忆存储
     * @param vector_store 向量存储
     * @param inputs 输入数据映射
     */
    static void build_loop_body(
        workflow::GraphBuilder& builder,
        const AgentConfig& agent_config,
        std::shared_ptr<LLMClient> llm_client,
        std::shared_ptr<ToolBus> toolbus,
        std::shared_ptr<MemoryStore> memory_store,
        std::shared_ptr<VectorStore> vector_store,
        const std::unordered_map<std::string, std::any>& inputs
    );
    
    /**
     * @brief 循环条件判断函数
     * @param outputs 循环体输出
     * @param iteration_count 当前迭代次数
     * @param max_iterations 最大迭代次数
     * @return 0 表示继续循环，非 0 表示退出
     */
    static int check_loop_condition(
        const std::unordered_map<std::string, std::any>& outputs,
        int iteration_count,
        int max_iterations
    );
    
    /**
     * @brief 构建退出处理（循环结束后的处理）
     * @param builder 子图构建器
     * @param inputs 输入数据映射
     */
    static void build_exit_handler(
        workflow::GraphBuilder& builder,
        const std::unordered_map<std::string, std::any>& inputs
    );
};

} // namespace node
} // namespace agent_framework

#endif // __AGENT_NODE_AGENT_LOOP_NODE_H__
