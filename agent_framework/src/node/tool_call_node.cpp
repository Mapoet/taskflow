/**
 * @file tool_call_node.cpp
 * @brief 工具调用节点封装实现
 */

#include "node/tool_call_node.hpp"

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
    const std::vector<std::pair<std::string, std::string>>& input_specs,
    const ToolOrchestrationOptions& orch_opts) {
    auto parallel_functor = [toolbus, orch_opts](
        const std::unordered_map<std::string, std::any>& inputs)
        -> std::unordered_map<std::string, std::any> {
        std::vector<CallSpec> call_list =
            std::any_cast<std::vector<CallSpec>>(inputs.at("call_list"));
        auto classify = [toolbus](std::string_view nm) -> ToolSideEffect {
            return toolbus->get_tool_meta(std::string(nm)).side_effect;
        };
        std::vector<json> raw =
            execute_tool_calls_sequenced(toolbus, call_list, orch_opts, classify);
        std::vector<json> results;
        results.reserve(raw.size());
        std::vector<std::string> tool_names;
        tool_names.reserve(call_list.size());
        for (std::size_t i = 0; i < raw.size(); ++i) {
            json one = std::move(raw[i]);
            one["tool_name"] = call_list[i].name;
            tool_names.push_back(call_list[i].name);
            results.push_back(std::move(one));
        }
        return {
            {"results", std::any{std::move(results)}},
            {"tool_names", std::any{std::move(tool_names)}}
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

} // namespace node
} // namespace agent_framework

