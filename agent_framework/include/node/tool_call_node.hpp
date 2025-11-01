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
     * @brief 创建并行工具调用节点（使用 create_for_each）
     * @param builder 图构建器
     * @param name 节点名称
     * @param toolbus ToolBus 实例
     * @param input_specs 输入规格：
     *   - {"ToolCallList", "call_list"} - 工具调用列表（std::vector<CallSpec>）
     * @return (节点指针, 任务句柄)
     * 
     * 输出键：
     *   - "results": 所有工具执行结果列表（std::vector<json>）
     *   - "tool_names": 工具名称列表（std::vector<std::string>）
     */
    static std::pair<std::shared_ptr<workflow::AnyNode>, tf::Task>
    create_parallel(
        workflow::GraphBuilder& builder,
        const std::string& name,
        std::shared_ptr<ToolBus> toolbus,
        const std::vector<std::pair<std::string, std::string>>& input_specs
    );

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
    
    /**
     * @brief 执行并行工具调用（for_each 回调）
     * @param call_spec 单个工具调用规范
     * @param shared_results 共享结果列表（由 for_each 提供）
     */
    static void execute_parallel_tool(
        const CallSpec& call_spec,
        std::shared_ptr<std::vector<json>> shared_results,
        std::shared_ptr<std::mutex> results_mutex,
        std::shared_ptr<ToolBus> toolbus
    );
};

} // namespace node
} // namespace agent_framework

#endif // __AGENT_NODE_TOOL_CALL_NODE_H__

