/**
 * @file llm_node.hpp
 * @brief LLM 节点封装：将 LLM 调用封装为 workflow 节点
 * @author Mapoet
 * @version 0.1
 * @date 2025-01-XX
 */
#ifndef __AGENT_NODE_LLM_NODE_H__
#define __AGENT_NODE_LLM_NODE_H__

#include <workflow/nodeflow.hpp>
#include "../agent/types.hpp"
#include "../agent/llm_client.hpp"
#include <string>
#include <memory>
#include <functional>

namespace agent_framework {
namespace node {

/**
 * @brief LLM 节点封装类
 * 将 LLMClient 调用封装为 workflow AnyNode
 */
class LLMNode {
public:
    /**
     * @brief 创建 LLM 节点
     * @param builder 图构建器
     * @param name 节点名称
     * @param llm_client LLM 客户端
     * @param input_specs 输入规格：
     *   - {"SystemPrompt", "prompt"} - 系统提示词
     *   - {"UserInput", "query"} - 用户输入
     *   - {"KnowledgeBase", "context"} - 知识库上下文（可选）
     *   - {"Memory", "history"} - 对话历史（可选）
     *   - {"ToolList", "tools"} - 工具列表（可选）
     *   - {"ImageInput", "image_data"} - 图像输入（可选）
     *   - {"AudioInput", "audio_data"} - 音频输入（可选）
     * @param stream_callback 流式输出回调（可选）
     * @return (节点指针, 任务句柄)
     */
    static std::pair<std::shared_ptr<workflow::AnyNode>, tf::Task>
    create(
        workflow::GraphBuilder& builder,
        const std::string& name,
        std::shared_ptr<LLMClient> llm_client,
        std::shared_ptr<PromptRenderer> prompt_renderer,
        const std::string& model_name,
        const std::string& provider,
        const std::vector<std::pair<std::string, std::string>>& input_specs,
        std::function<void(std::string_view)> stream_callback = nullptr
    );

private:
    /**
     * @brief 从输入映射中提取 LLMInput
     * @param inputs 输入映射
     * @return LLMInput 结构
     */
    static LLMInput extract_llm_input(
        const std::unordered_map<std::string, std::any>& inputs
    );
};

} // namespace node
} // namespace agent_framework

#endif // __AGENT_NODE_LLM_NODE_H__

