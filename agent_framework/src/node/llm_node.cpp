/**
 * @file llm_node.cpp
 * @brief LLM 节点封装实现
 */

#include "node/llm_node.hpp"
#include "agent/internal/agent_thread_state.hpp"
#include "agent/internal/loop_io_keys.hpp"
#include <any>
#include <unordered_map>

namespace agent_framework {
namespace node {

std::pair<std::shared_ptr<workflow::AnyNode>, tf::Task>
LLMNode::create(
    workflow::GraphBuilder& builder,
    const std::string& name,
    std::shared_ptr<LLMClient> llm_client,
    std::shared_ptr<PromptRenderer> prompt_renderer,
    const std::string& model_name,
    const std::string& provider,
    const std::vector<std::pair<std::string, std::string>>& input_specs,
    std::function<void(std::string_view)> stream_callback
) {
    // 创建 LLM 节点的 functor
    auto llm_functor = [llm_client, prompt_renderer, model_name, provider, stream_callback](
        const std::unordered_map<std::string, std::any>& inputs
    ) -> std::unordered_map<std::string, std::any> {
        // 1. 提取 LLMInput
        LLMInput llm_input = extract_llm_input(inputs);

        // 2. 渲染提示词（两阶段渲染在 PromptRenderer 内）
        if (!prompt_renderer) {
            throw std::runtime_error("LLMNode: prompt_renderer is null");
        }
        RenderedPrompt rendered = prompt_renderer->render(llm_input, model_name);
        if (rendered.context_budget_blocked) {
            LLMOutput blocked_out;
            blocked_out.is_final = true;
            blocked_out.final_answer =
                "[context_budget] blocked: AGENT_CONTEXT_BUDGET_STRICT and combined budget still exceeded "
                "after truncation";
            return std::unordered_map<std::string, std::any>{
                {std::string(internal::kLlmOutput), std::any{std::move(blocked_out)}}};
        }

        // 3. 调用 LLM（使用已渲染提示词）
        std::future<LLMOutput> future =
            llm_client->invoke_with_rendered_prompt(rendered, provider, stream_callback);
        LLMOutput output = future.get();

        return std::unordered_map<std::string, std::any>{
            {std::string(internal::kLlmOutput), std::any{output}}
        };
    };
    
    // 使用 GraphBuilder 创建节点
    return builder.create_any_node(
        name,
        input_specs,
        llm_functor,
        {std::string(internal::kLlmOutput)}
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
    if (inputs.find(std::string(internal::kAgentState)) != inputs.end()) {
        auto st = std::any_cast<std::shared_ptr<internal::AgentThreadState>>(
            inputs.at(std::string(internal::kAgentState)));
        if (st) {
            llm_input.history = st->history;
            if (!st->initial_user_prompt.empty()) {
                llm_input.user_prompt = st->initial_user_prompt;
            }
        }
    }
    
    // 提取图像数据
    if (inputs.find("image_data") != inputs.end()) {
        llm_input.image_data = std::any_cast<std::string>(inputs.at("image_data"));
    }
    
    // 提取音频数据
    if (inputs.find("audio_data") != inputs.end()) {
        llm_input.audio_data = std::any_cast<std::string>(inputs.at("audio_data"));
    }

    if (inputs.find("extra_variables") != inputs.end()) {
        llm_input.extra_variables =
            std::any_cast<std::map<std::string, std::string>>(inputs.at("extra_variables"));
    }
    
    return llm_input;
}

} // namespace node
} // namespace agent_framework

