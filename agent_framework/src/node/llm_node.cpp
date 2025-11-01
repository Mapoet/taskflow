/**
 * @file llm_node.cpp
 * @brief LLM 节点封装实现
 */

#include "node/llm_node.hpp"
#include <any>
#include <unordered_map>

namespace agent_framework {
namespace node {

std::pair<std::shared_ptr<workflow::AnyNode>, tf::Task>
LLMNode::create(
    workflow::GraphBuilder& builder,
    const std::string& name,
    std::shared_ptr<LLMClient> llm_client,
    const std::vector<std::pair<std::string, std::string>>& input_specs,
    std::function<void(std::string_view)> stream_callback
) {
    // 创建 LLM 节点的 functor
    auto llm_functor = [llm_client, stream_callback](
        const std::unordered_map<std::string, std::any>& inputs
    ) -> std::unordered_map<std::string, std::any> {
        // 1. 提取 LLMInput
        LLMInput llm_input = extract_llm_input(inputs);
        
        // 2. 调用 LLM（异步，但这里同步等待结果）
        std::future<LLMOutput> future = llm_client->invoke(
            llm_input,
            "",  // 使用默认 provider
            stream_callback
        );
        LLMOutput output = future.get();
        
        // 3. 返回输出
        return std::unordered_map<std::string, std::any>{
            {"tool_calls", std::any{output.tool_calls}},
            {"reasoning", std::any{output.reasoning}},
            {"is_final", std::any{output.is_final}},
            {"final_answer", std::any{output.final_answer}},
            {"audio_out", std::any{output.audio_out.value_or("")}}
        };
    };
    
    // 使用 GraphBuilder 创建节点
    return builder.create_any_node(
        name,
        input_specs,
        llm_functor,
        {"tool_calls", "reasoning", "is_final", "final_answer", "audio_out"}
    );
}

LLMInput LLMNode::extract_llm_input(
    const std::unordered_map<std::string, std::any>& inputs
) {
    LLMInput llm_input;
    
    // 提取系统提示词
    if (inputs.find("prompt") != inputs.end()) {
        llm_input.system_prompt = std::any_cast<std::string>(inputs.at("prompt"));
    }
    
    // 提取用户输入
    if (inputs.find("query") != inputs.end()) {
        llm_input.user_prompt = std::any_cast<std::string>(inputs.at("query"));
    }
    
    // 提取知识库上下文
    if (inputs.find("context") != inputs.end()) {
        llm_input.context = std::any_cast<std::string>(inputs.at("context"));
    }
    
    // 提取工具列表
    if (inputs.find("tools") != inputs.end()) {
        llm_input.tools = std::any_cast<std::vector<ToolMeta>>(inputs.at("tools"));
    }
    
    // 提取对话历史
    if (inputs.find("history") != inputs.end()) {
        llm_input.history = std::any_cast<std::vector<Message>>(inputs.at("history"));
    }
    
    // 提取图像数据
    if (inputs.find("image_data") != inputs.end()) {
        llm_input.image_data = std::any_cast<std::string>(inputs.at("image_data"));
    }
    
    // 提取音频数据
    if (inputs.find("audio_data") != inputs.end()) {
        llm_input.audio_data = std::any_cast<std::string>(inputs.at("audio_data"));
    }
    
    return llm_input;
}

} // namespace node
} // namespace agent_framework

