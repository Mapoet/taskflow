/**
 * @file node_factory.cpp
 * @brief 节点工厂实现
 */

#include "node/node_factory.hpp"
#include "node/llm_node.hpp"
#include "node/knowledge_base_node.hpp"
#include "node/tool_call_node.hpp"
#include "node/agent_loop_node.hpp"
#include "node/ui_sink_node.hpp"

namespace agent_framework {
namespace node {

std::pair<std::shared_ptr<workflow::AnyNode>, tf::Task>
NodeFactory::create_llm_node(
    workflow::GraphBuilder& builder,
    const std::string& name,
    std::shared_ptr<LLMClient> llm_client,
    const std::vector<std::pair<std::string, std::string>>& input_specs,
    std::function<void(std::string_view)> stream_callback
) {
    // Default factory path: use LLMClient internal renderer; for WP1.5, prefer passing renderer explicitly.
    return LLMNode::create(builder, name, llm_client, std::make_shared<PromptRenderer>(),
                           llm_client->get_model_name(), "", input_specs, stream_callback);
}

std::pair<std::shared_ptr<workflow::AnySource>, tf::Task>
NodeFactory::create_knowledge_base_source(
    workflow::GraphBuilder& builder,
    const std::string& name,
    std::shared_ptr<VectorStore> vector_store,
    std::shared_ptr<EncoderManager> encoder_manager
) {
    return KnowledgeBaseSourceNode::create(builder, name, vector_store, encoder_manager);
}

std::pair<std::shared_ptr<workflow::AnyNode>, tf::Task>
NodeFactory::create_tool_call_node(
    workflow::GraphBuilder& builder,
    const std::string& name,
    std::shared_ptr<ToolBus> toolbus,
    const std::vector<std::pair<std::string, std::string>>& input_specs
) {
    return ToolCallNode::create_single(builder, name, toolbus, input_specs);
}

std::pair<std::shared_ptr<workflow::AnyNode>, tf::Task>
NodeFactory::create_parallel_tool_call_node(
    workflow::GraphBuilder& builder,
    const std::string& name,
    std::shared_ptr<ToolBus> toolbus,
    const std::vector<std::pair<std::string, std::string>>& input_specs
) {
    return ToolCallNode::create_parallel(builder, name, toolbus, input_specs);
}

std::pair<std::shared_ptr<workflow::AnyNode>, tf::Task>
NodeFactory::create_prompt_render_node(
    workflow::GraphBuilder& builder,
    const std::string& name,
    std::shared_ptr<PromptRenderer> prompt_renderer,
    const std::string& model_name,
    const std::vector<std::pair<std::string, std::string>>& input_specs
) {
    // TODO: 实现提示词渲染节点
    // 这需要从输入中提取 LLMInput，调用 PromptRenderer，返回 RenderedPrompt
    throw std::runtime_error("create_prompt_render_node not yet implemented");
}

std::pair<std::shared_ptr<workflow::AnyNode>, tf::Task>
NodeFactory::create_text_encoder_node(
    workflow::GraphBuilder& builder,
    const std::string& name,
    std::shared_ptr<TextEncoder> encoder,
    const std::vector<std::pair<std::string, std::string>>& input_specs
) {
    // TODO: 实现文本编码器节点
    throw std::runtime_error("create_text_encoder_node not yet implemented");
}

std::pair<std::shared_ptr<workflow::AnyNode>, tf::Task>
NodeFactory::create_image_encoder_node(
    workflow::GraphBuilder& builder,
    const std::string& name,
    std::shared_ptr<ImageEncoder> encoder,
    const std::vector<std::pair<std::string, std::string>>& input_specs
) {
    // TODO: 实现图像编码器节点
    throw std::runtime_error("create_image_encoder_node not yet implemented");
}

std::pair<std::shared_ptr<workflow::AnyNode>, tf::Task>
NodeFactory::create_audio_encoder_node(
    workflow::GraphBuilder& builder,
    const std::string& name,
    std::shared_ptr<AudioEncoder> encoder,
    const std::vector<std::pair<std::string, std::string>>& input_specs
) {
    // TODO: 实现音频编码器节点
    throw std::runtime_error("create_audio_encoder_node not yet implemented");
}

std::pair<std::shared_ptr<workflow::AnySink>, tf::Task>
NodeFactory::create_vector_store_sink(
    workflow::GraphBuilder& builder,
    const std::string& name,
    std::shared_ptr<VectorStore> vector_store,
    const std::vector<std::pair<std::string, std::string>>& input_specs
) {
    // TODO: 实现向量存储 Sink 节点
    throw std::runtime_error("create_vector_store_sink not yet implemented");
}

std::pair<std::shared_ptr<workflow::AnySink>, tf::Task>
NodeFactory::create_cli_sink(
    workflow::GraphBuilder& builder,
    const std::string& name,
    std::shared_ptr<UIManager> ui_manager,
    const std::vector<std::pair<std::string, std::string>>& input_specs
) {
    return UISinkNode::create_cli(builder, name, ui_manager, input_specs);
}

std::pair<std::shared_ptr<workflow::AnySink>, tf::Task>
NodeFactory::create_imgui_sink(
    workflow::GraphBuilder& builder,
    const std::string& name,
    std::shared_ptr<UIManager> ui_manager,
    const std::vector<std::pair<std::string, std::string>>& input_specs
) {
    return UISinkNode::create_imgui(builder, name, ui_manager, input_specs);
}

std::pair<std::shared_ptr<workflow::AnySink>, tf::Task>
NodeFactory::create_web_sink(
    workflow::GraphBuilder& builder,
    const std::string& name,
    std::shared_ptr<UIManager> ui_manager,
    const std::string& session_id,
    const std::vector<std::pair<std::string, std::string>>& input_specs
) {
    return UISinkNode::create_web(builder, name, ui_manager, session_id, input_specs);
}

std::pair<std::shared_ptr<workflow::AnySink>, tf::Task>
NodeFactory::create_memory_sink(
    workflow::GraphBuilder& builder,
    const std::string& name,
    std::shared_ptr<MemoryStore> memory_store,
    const std::string& session_id,
    const std::vector<std::pair<std::string, std::string>>& input_specs
) {
    // TODO: 实现记忆存储 Sink 节点
    throw std::runtime_error("create_memory_sink not yet implemented");
}

std::pair<std::shared_ptr<workflow::LoopNode>, tf::Task>
NodeFactory::create_agent_loop_node(
    workflow::GraphBuilder& builder,
    const std::string& name,
    const AgentConfig& agent_config,
    std::shared_ptr<LLMClient> llm_client,
    std::shared_ptr<ToolBus> toolbus,
    std::shared_ptr<MemoryStore> memory_store,
    std::shared_ptr<VectorStore> vector_store,
    const std::vector<std::pair<std::string, std::string>>& input_specs,
    const std::vector<std::string>& output_keys
) {
    return AgentLoopNode::create(
        builder,
        name,
        agent_config,
        llm_client,
        toolbus,
        memory_store,
        vector_store,
        input_specs,
        output_keys,
        nullptr
    );
}

} // namespace node
} // namespace agent_framework

