/**
 * @file tool_call_node.hpp
 * @brief 工具调用节点封装：将工具调用封装为 workflow 节点
 * @author Mapoet
 * @version 0.1
 * @date 2025-01-XX
 */
#ifndef __AGENT_NODE_TOOL_CALL_NODE_H__
#define __AGENT_NODE_TOOL_CALL_NODE_H__

#include <workflow/nodeflow.hpp>
#include "../agent/types.hpp"
#include "../agent/toolbus.hpp"
#include <string>
#include <memory>
#include <vector>

namespace agent_framework {
namespace node {

/**
 * @brief 工具调用节点封装类
 * 将 ToolBus 工具调用封装为 workflow AnyNode
 */
class ToolCallNode {
public:
    /**
     * @brief 创建单个工具调用节点
     * @param builder 图构建器
     * @param name 节点名称
     * @param toolbus ToolBus 实例
     * @param input_specs 输入规格：
     *   - {"ToolCall", "call_spec"} - 工具调用规范（CallSpec 结构）
     * @return (节点指针, 任务句柄)
     * 
     * 输出键：
     *   - "result": 工具执行结果（JSON 格式）
     *   - "tool_name": 工具名称（字符串）
     */
    static std::pair<std::shared_ptr<workflow::AnyNode>, tf::Task>
    create_single(
        workflow::GraphBuilder& builder,
        const std::string& name,
        std::shared_ptr<ToolBus> toolbus,
        const std::vector<std::pair<std::string, std::string>>& input_specs
    );
    
    /**
     * @brief 创建多工具调用节点（WP2.1b：`execute_tool_calls_sequenced`）
     *
     * 默认 `orch_opts.enable_parallel_reads == false` 时顺序执行。
     * `orch_opts.enable_parallel_reads == true` 时仅对连续 `ReadOnly` 工具并行（受 `max_parallel_reads` 限制）。
     *
     * 输出：每个 result JSON 含 `tool_name` 字段（与历史兼容）。
     */
    static std::pair<std::shared_ptr<workflow::AnyNode>, tf::Task>
    create_parallel(
        workflow::GraphBuilder& builder,
        const std::string& name,
        std::shared_ptr<ToolBus> toolbus,
        const std::vector<std::pair<std::string, std::string>>& input_specs,
        const ToolOrchestrationOptions& orch_opts = ToolOrchestrationOptions{});

private:
    /**
     * @brief 执行单个工具调用
     * @param call_spec 工具调用规范
     * @param toolbus ToolBus 实例
     * @return 执行结果
     */
    static std::unordered_map<std::string, std::any> execute_single_tool(
        const CallSpec& call_spec,
        std::shared_ptr<ToolBus> toolbus
    );
};

} // namespace node
} // namespace agent_framework

#endif // __AGENT_NODE_TOOL_CALL_NODE_H__

