/**
 * @file node_factory.hpp
 * @brief 节点工厂：提供便捷的节点创建接口，封装 Agent 功能为 workflow 节点
 * @author Mapoet
 * @version 0.1
 * @date 2025-01-XX
 */
#ifndef __AGENT_NODE_FACTORY_H__
#define __AGENT_NODE_FACTORY_H__

#include <workflow/nodeflow.hpp>
#include "../agent/types.hpp"
#include "../agent/llm_client.hpp"
#include "../agent/prompt_renderer.hpp"
#include "../agent/toolbus.hpp"
#include "../agent/memory.hpp"
#include "../agent/vectorstore.hpp"
#include "../agent/encoder.hpp"
#include "../agent/ui_manager.hpp"
#include <string>
#include <memory>
#include <functional>

namespace agent_framework {

// 前向声明
class LLMClient;
class PromptRenderer;
class ToolBus;
class MemoryStore;
class VectorStore;
class EncoderManager;
class UIManager;

namespace node {

// ============================================================================
// 节点工厂类
// ============================================================================

/**
 * @brief 节点工厂类
 * 提供便捷的方法来创建各种 Agent 功能节点
 */
class NodeFactory {
public:
    /**
     * @brief 创建 LLM 节点
     * @param builder 图构建器
     * @param name 节点名称
     * @param llm_client LLM 客户端
     * @param input_specs 输入规格 {{"源节点", "输出键"}, ...}
     * @param stream_callback 流式输出回调（可选）
     * @return (节点指针, 任务句柄)
     */
    static std::pair<std::shared_ptr<workflow::AnyNode>, tf::Task>
    create_llm_node(
        workflow::GraphBuilder& builder,
        const std::string& name,
        std::shared_ptr<LLMClient> llm_client,
        const std::vector<std::pair<std::string, std::string>>& input_specs,
        std::function<void(std::string_view)> stream_callback = nullptr
    );
    
    /**
     * @brief 创建知识库 Source 节点
     * @param builder 图构建器
     * @param name 节点名称
     * @param vector_store 向量存储
     * @param encoder_manager 编码器管理器
     * @return (节点指针, 任务句柄)
     */
    static std::pair<std::shared_ptr<workflow::AnySource>, tf::Task>
    create_knowledge_base_source(
        workflow::GraphBuilder& builder,
        const std::string& name,
        std::shared_ptr<VectorStore> vector_store,
        std::shared_ptr<EncoderManager> encoder_manager
    );
    
    /**
     * @brief 创建工具调用节点
     * @param builder 图构建器
     * @param name 节点名称
     * @param toolbus ToolBus 实例
     * @param input_specs 输入规格 {{"源节点", "输出键"}, ...}
     * @return (节点指针, 任务句柄)
     */
    static std::pair<std::shared_ptr<workflow::AnyNode>, tf::Task>
    create_tool_call_node(
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
     * @param input_specs 输入规格 {{"源节点", "输出键"}, ...}
     * @return (节点指针, 任务句柄)
     */
    static std::pair<std::shared_ptr<workflow::AnyNode>, tf::Task>
    create_parallel_tool_call_node(
        workflow::GraphBuilder& builder,
        const std::string& name,
        std::shared_ptr<ToolBus> toolbus,
        const std::vector<std::pair<std::string, std::string>>& input_specs
    );
    
    /**
     * @brief 创建提示词渲染节点
     * @param builder 图构建器
     * @param name 节点名称
     * @param prompt_renderer 提示词渲染器
     * @param model_name 模型名称
     * @param input_specs 输入规格
     * @return (节点指针, 任务句柄)
     */
    static std::pair<std::shared_ptr<workflow::AnyNode>, tf::Task>
    create_prompt_render_node(
        workflow::GraphBuilder& builder,
        const std::string& name,
        std::shared_ptr<PromptRenderer> prompt_renderer,
        const std::string& model_name,
        const std::vector<std::pair<std::string, std::string>>& input_specs
    );
    
    /**
     * @brief 创建文本编码器节点
     * @param builder 图构建器
     * @param name 节点名称
     * @param encoder 文本编码器
     * @param input_specs 输入规格
     * @return (节点指针, 任务句柄)
     */
    static std::pair<std::shared_ptr<workflow::AnyNode>, tf::Task>
    create_text_encoder_node(
        workflow::GraphBuilder& builder,
        const std::string& name,
        std::shared_ptr<TextEncoder> encoder,
        const std::vector<std::pair<std::string, std::string>>& input_specs
    );
    
    /**
     * @brief 创建图像编码器节点
     * @param builder 图构建器
     * @param name 节点名称
     * @param encoder 图像编码器
     * @param input_specs 输入规格
     * @return (节点指针, 任务句柄)
     */
    static std::pair<std::shared_ptr<workflow::AnyNode>, tf::Task>
    create_image_encoder_node(
        workflow::GraphBuilder& builder,
        const std::string& name,
        std::shared_ptr<ImageEncoder> encoder,
        const std::vector<std::pair<std::string, std::string>>& input_specs
    );
    
    /**
     * @brief 创建音频编码器节点
     * @param builder 图构建器
     * @param name 节点名称
     * @param encoder 音频编码器
     * @param input_specs 输入规格
     * @return (节点指针, 任务句柄)
     */
    static std::pair<std::shared_ptr<workflow::AnyNode>, tf::Task>
    create_audio_encoder_node(
        workflow::GraphBuilder& builder,
        const std::string& name,
        std::shared_ptr<AudioEncoder> encoder,
        const std::vector<std::pair<std::string, std::string>>& input_specs
    );
    
    /**
     * @brief 创建向量存储 Sink 节点
     * @param builder 图构建器
     * @param name 节点名称
     * @param vector_store 向量存储
     * @param input_specs 输入规格
     * @return (节点指针, 任务句柄)
     */
    static std::pair<std::shared_ptr<workflow::AnySink>, tf::Task>
    create_vector_store_sink(
        workflow::GraphBuilder& builder,
        const std::string& name,
        std::shared_ptr<VectorStore> vector_store,
        const std::vector<std::pair<std::string, std::string>>& input_specs
    );
    
    /**
     * @brief 创建 CLI 输出 Sink 节点
     * @param builder 图构建器
     * @param name 节点名称
     * @param ui_manager UI 管理器
     * @param input_specs 输入规格
     * @return (节点指针, 任务句柄)
     */
    static std::pair<std::shared_ptr<workflow::AnySink>, tf::Task>
    create_cli_sink(
        workflow::GraphBuilder& builder,
        const std::string& name,
        std::shared_ptr<UIManager> ui_manager,
        const std::vector<std::pair<std::string, std::string>>& input_specs
    );
    
    /**
     * @brief 创建 ImGui 输出 Sink 节点
     * @param builder 图构建器
     * @param name 节点名称
     * @param ui_manager UI 管理器
     * @param input_specs 输入规格
     * @return (节点指针, 任务句柄)
     */
    static std::pair<std::shared_ptr<workflow::AnySink>, tf::Task>
    create_imgui_sink(
        workflow::GraphBuilder& builder,
        const std::string& name,
        std::shared_ptr<UIManager> ui_manager,
        const std::vector<std::pair<std::string, std::string>>& input_specs
    );
    
    /**
     * @brief 创建 Web 输出 Sink 节点（SSE/WebSocket）
     * @param builder 图构建器
     * @param name 节点名称
     * @param ui_manager UI 管理器
     * @param session_id 会话 ID
     * @param input_specs 输入规格
     * @return (节点指针, 任务句柄)
     */
    static std::pair<std::shared_ptr<workflow::AnySink>, tf::Task>
    create_web_sink(
        workflow::GraphBuilder& builder,
        const std::string& name,
        std::shared_ptr<UIManager> ui_manager,
        const std::string& session_id,
        const std::vector<std::pair<std::string, std::string>>& input_specs
    );
    
    /**
     * @brief 创建记忆存储节点
     * @param builder 图构建器
     * @param name 节点名称
     * @param memory_store 记忆存储
     * @param session_id 会话 ID
     * @param input_specs 输入规格
     * @return (节点指针, 任务句柄)
     */
    static std::pair<std::shared_ptr<workflow::AnySink>, tf::Task>
    create_memory_sink(
        workflow::GraphBuilder& builder,
        const std::string& name,
        std::shared_ptr<MemoryStore> memory_store,
        const std::string& session_id,
        const std::vector<std::pair<std::string, std::string>>& input_specs
    );
    
    /**
     * @brief 创建 Agent 循环节点（封装整个 Agent 循环为可复用节点）
     * @param builder 图构建器
     * @param name 节点名称
     * @param agent_config Agent 配置
     * @param llm_client LLM 客户端
     * @param toolbus ToolBus 实例
     * @param memory_store 记忆存储
     * @param vector_store 向量存储
     * @param input_specs 输入规格
     * @param output_keys 输出键列表
     * @return (节点指针, 任务句柄)
     */
    static std::pair<std::shared_ptr<workflow::LoopNode>, tf::Task>
    create_agent_loop_node(
        workflow::GraphBuilder& builder,
        const std::string& name,
        const AgentConfig& agent_config,
        std::shared_ptr<LLMClient> llm_client,
        std::shared_ptr<ToolBus> toolbus,
        std::shared_ptr<MemoryStore> memory_store,
        std::shared_ptr<VectorStore> vector_store,
        const std::vector<std::pair<std::string, std::string>>& input_specs,
        const std::vector<std::string>& output_keys
    );
};

} // namespace node
} // namespace agent_framework

#endif // __AGENT_NODE_FACTORY_H__

