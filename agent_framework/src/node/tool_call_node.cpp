/**
 * @file tool_call_node.cpp
 * @brief 工具调用节点封装实现
 */

#include "node/tool_call_node.hpp"
#include <mutex>
#include <vector>

namespace agent_framework {
namespace node {

std::pair<std::shared_ptr<workflow::AnyNode>, tf::Task>
ToolCallNode::create_single(
    workflow::GraphBuilder& builder,
    const std::string& name,
    std::shared_ptr<ToolBus> toolbus,
    const std::vector<std::pair<std::string, std::string>>& input_specs
) {
    auto tool_functor = [toolbus](
        const std::unordered_map<std::string, std::any>& inputs
    ) -> std::unordered_map<std::string, std::any> {
        // 提取工具调用规范
        CallSpec call_spec = std::any_cast<CallSpec>(inputs.at("call_spec"));
        
        // 执行工具调用
        return execute_single_tool(call_spec, toolbus);
    };
    
    return builder.create_any_node(
        name,
        input_specs,
        tool_functor,
        {"result", "tool_name"}
    );
}

std::pair<std::shared_ptr<workflow::AnyNode>, tf::Task>
ToolCallNode::create_parallel(
    workflow::GraphBuilder& builder,
    const std::string& name,
    std::shared_ptr<ToolBus> toolbus,
    const std::vector<std::pair<std::string, std::string>>& input_specs
) {
    // 创建共享状态
    auto shared_results = std::make_shared<std::vector<json>>();
    auto results_mutex = std::make_shared<std::mutex>();
    
    // 使用普通节点实现并行工具调用（等待所有调用完成）
    auto parallel_functor = [toolbus, shared_results, results_mutex](
        const std::unordered_map<std::string, std::any>& inputs
    ) -> std::unordered_map<std::string, std::any> {
        // 提取工具调用列表
        std::vector<CallSpec> call_list = std::any_cast<std::vector<CallSpec>>(
            inputs.at("call_list")
        );
        
        // 并行执行所有工具调用（使用 std::async）
        std::vector<std::future<json>> futures;
        for (const auto& call_spec : call_list) {
            auto future = toolbus->call_tool(call_spec.name, call_spec.arguments);
            futures.push_back(std::move(future));
        }
        
        // 收集所有结果
        std::vector<std::string> tool_names;
        for (size_t i = 0; i < futures.size(); ++i) {
            json result = futures[i].get();
            result["tool_name"] = call_list[i].name;
            shared_results->push_back(result);
            tool_names.push_back(call_list[i].name);
        }
        
        return {
            {"results", std::any{*shared_results}},
            {"tool_names", std::any{tool_names}}
        };
    };
    
    return builder.create_any_node(
        name,
        input_specs,
        parallel_functor,
        {"results", "tool_names"}
    );
}

std::unordered_map<std::string, std::any> ToolCallNode::execute_single_tool(
    const CallSpec& call_spec,
    std::shared_ptr<ToolBus> toolbus
) {
    // 调用工具
    auto future = toolbus->call_tool(call_spec.name, call_spec.arguments);
    json result = future.get();
    
    return {
        {"result", std::any{result}},
        {"tool_name", std::any{call_spec.name}}
    };
}

void ToolCallNode::execute_parallel_tool(
    const CallSpec& call_spec,
    std::shared_ptr<std::vector<json>> shared_results,
    std::shared_ptr<std::mutex> results_mutex,
    std::shared_ptr<ToolBus> toolbus
) {
    // 调用工具
    auto future = toolbus->call_tool(call_spec.name, call_spec.arguments);
    json result = future.get();
    
    // 添加工具名称到结果中
    result["tool_name"] = call_spec.name;
    
    // 线程安全地添加到共享结果列表
    {
        std::lock_guard<std::mutex> lock(*results_mutex);
        shared_results->push_back(result);
    }
}

} // namespace node
} // namespace agent_framework

