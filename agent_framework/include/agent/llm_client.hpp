/**
 * @file llm_client.hpp
 * @brief LLM Client 模块：多模型适配器和管理器
 * @author Mapoet
 * @version 0.1
 * @date 2025-01-XX
 */
#ifndef __AGENT_LLM_CLIENT_H__
#define __AGENT_LLM_CLIENT_H__

#include "types.hpp"
#include "prompt_renderer.hpp"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <future>
#include <mutex>

namespace agent_framework {

// ============================================================================
// 模型适配器接口
// ============================================================================

/**
 * @brief 模型适配器虚基类
 * 定义统一的 LLM 调用接口，支持多种模型提供商
 */
class ModelAdapter {
public:
    virtual ~ModelAdapter() = default;
    
    /**
     * @brief 异步调用 LLM，支持流式输出
     * @note 内部会将 LLMInput 转换为 RenderedPrompt，再构建 API 请求
     * @param input LLM 输入
     * @param stream_callback 流式输出回调函数（可选）
     * @return LLM 输出（异步 future）
     */
    virtual std::future<LLMOutput> invoke(
        const LLMInput& input,
        std::function<void(std::string_view)> stream_callback = nullptr
    ) = 0;
    
    /**
     * @brief 使用已渲染的提示词调用 LLM（高级接口）
     * @note 直接接收 RenderedPrompt，跳过渲染步骤
     * @param rendered 渲染后的提示词
     * @param stream_callback 流式输出回调函数（可选）
     * @return LLM 输出（异步 future）
     */
    virtual std::future<LLMOutput> invoke_with_rendered(
        const RenderedPrompt& rendered,
        std::function<void(std::string_view)> stream_callback = nullptr
    ) = 0;
    
    /**
     * @brief 获取模型支持的工具列表
     * @return 工具元数据列表
     */
    virtual std::vector<ToolMeta> get_available_tools() const = 0;
    
    /**
     * @brief 配置模型参数
     * @param config 模型配置
     */
    virtual void configure(const ModelConfig& config) = 0;
    
    /**
     * @brief 获取模型名称
     * @return 模型名称（如 "gpt-4o", "claude-3-opus"）
     */
    virtual std::string get_model_name() const = 0;
    
    /**
     * @brief 检查模型是否支持多模态输入
     * @return true 如果支持多模态
     */
    virtual bool supports_multimodal() const = 0;
    
protected:
    /**
     * @brief 公共的错误处理和重试逻辑（可由派生类调用）
     * @param endpoint API 端点
     * @param payload 请求载荷
     * @return 响应 JSON
     */
    virtual json send_request(const std::string& endpoint, const json& payload);
    
    /**
     * @brief 解析响应（可由派生类调用）
     * @param response 响应字符串
     * @return 解析后的 JSON
     */
    virtual json parse_response(const std::string& response);
};

/**
 * @brief OpenAI 适配器
 */
class OpenAIAdapter : public ModelAdapter {
public:
    explicit OpenAIAdapter(const std::string& api_key, 
                          const std::string& base_url = "https://api.openai.com/v1");
    
    std::future<LLMOutput> invoke(
        const LLMInput& input,
        std::function<void(std::string_view)> stream_callback = nullptr
    ) override;
    
    std::future<LLMOutput> invoke_with_rendered(
        const RenderedPrompt& rendered,
        std::function<void(std::string_view)> stream_callback = nullptr
    ) override;
    
    std::vector<ToolMeta> get_available_tools() const override;
    void configure(const ModelConfig& config) override;
    std::string get_model_name() const override;
    bool supports_multimodal() const override;
    
private:
    std::string api_key_;
    std::string base_url_;
    ModelConfig config_;
    
    /**
     * @brief 构建 OpenAI 格式的请求（使用 RenderedPrompt）
     * @param rendered 渲染后的提示词
     * @return OpenAI API 请求 JSON
     */
    json build_openai_request(const RenderedPrompt& rendered);
};

/**
 * @brief Anthropic 适配器
 */
class AnthropicAdapter : public ModelAdapter {
public:
    explicit AnthropicAdapter(const std::string& api_key);
    
    std::future<LLMOutput> invoke(
        const LLMInput& input,
        std::function<void(std::string_view)> stream_callback = nullptr
    ) override;
    
    std::future<LLMOutput> invoke_with_rendered(
        const RenderedPrompt& rendered,
        std::function<void(std::string_view)> stream_callback = nullptr
    ) override;
    
    std::vector<ToolMeta> get_available_tools() const override;
    void configure(const ModelConfig& config) override;
    std::string get_model_name() const override;
    bool supports_multimodal() const override;
    
private:
    std::string api_key_;
    ModelConfig config_;
    
    /**
     * @brief 构建 Anthropic 格式的请求（使用 RenderedPrompt）
     * @param rendered 渲染后的提示词
     * @return Anthropic API 请求 JSON
     */
    json build_anthropic_request(const RenderedPrompt& rendered);
};

/**
 * @brief Gemini 适配器
 */
class GeminiAdapter : public ModelAdapter {
public:
    explicit GeminiAdapter(const std::string& api_key);
    
    std::future<LLMOutput> invoke(
        const LLMInput& input,
        std::function<void(std::string_view)> stream_callback = nullptr
    ) override;
    
    std::future<LLMOutput> invoke_with_rendered(
        const RenderedPrompt& rendered,
        std::function<void(std::string_view)> stream_callback = nullptr
    ) override;
    
    std::vector<ToolMeta> get_available_tools() const override;
    void configure(const ModelConfig& config) override;
    std::string get_model_name() const override;
    bool supports_multimodal() const override;
    
private:
    std::string api_key_;
    ModelConfig config_;
    
    /**
     * @brief 构建 Gemini 格式的请求（使用 RenderedPrompt）
     * @param rendered 渲染后的提示词
     * @return Gemini API 请求 JSON
     */
    json build_gemini_request(const RenderedPrompt& rendered);
};

/**
 * @brief vLLM 本地适配器（本地部署的 vLLM 服务器）
 */
class vLLMAdapter : public ModelAdapter {
public:
    explicit vLLMAdapter(const std::string& endpoint = "http://localhost:8000/v1");
    
    std::future<LLMOutput> invoke(
        const LLMInput& input,
        std::function<void(std::string_view)> stream_callback = nullptr
    ) override;
    
    std::future<LLMOutput> invoke_with_rendered(
        const RenderedPrompt& rendered,
        std::function<void(std::string_view)> stream_callback = nullptr
    ) override;
    
    std::vector<ToolMeta> get_available_tools() const override;
    void configure(const ModelConfig& config) override;
    std::string get_model_name() const override;
    bool supports_multimodal() const override;
    
private:
    std::string endpoint_;
    ModelConfig config_;
    
    /**
     * @brief 构建 vLLM 格式的请求（使用 RenderedPrompt）
     * @param rendered 渲染后的提示词
     * @return vLLM API 请求 JSON
     */
    json build_vllm_request(const RenderedPrompt& rendered);
};

// ============================================================================
// LLM 客户端管理器
// ============================================================================

/**
 * @brief LLM 客户端管理器（使用适配器模式）
 */
class LLMClient {
public:
    /**
     * @brief 设置提示词渲染器（用于将 LLMInput 转换为 RenderedPrompt）
     * @param renderer 提示词渲染器
     */
    void set_prompt_renderer(std::shared_ptr<PromptRenderer> renderer);
    
    /**
     * @brief 注册模型适配器
     * @param provider 提供商名称（如 "openai", "anthropic"）
     * @param adapter 模型适配器
     */
    void register_adapter(const std::string& provider, 
                         std::shared_ptr<ModelAdapter> adapter);
    
    /**
     * @brief 设置默认适配器
     * @param provider 提供商名称
     */
    void set_default_adapter(const std::string& provider);
    
    /**
     * @brief 调用 LLM（使用默认适配器或指定适配器）
     * @note 内部会自动使用 PromptRenderer 渲染提示词
     * @param input LLM 输入
     * @param provider 提供商名称（空字符串表示使用默认）
     * @param stream_callback 流式输出回调函数
     * @return LLM 输出（异步 future）
     */
    std::future<LLMOutput> invoke(
        const LLMInput& input,
        const std::string& provider = "",
        std::function<void(std::string_view)> stream_callback = nullptr
    );
    
    /**
     * @brief 使用渲染后的提示词调用 LLM（高级接口，跳过渲染步骤）
     * @note 适用于已经渲染好的提示词，或在 LLM 节点中已经渲染的情况
     * @param rendered 渲染后的提示词
     * @param provider 提供商名称（空字符串表示使用默认）
     * @param stream_callback 流式输出回调函数
     * @return LLM 输出（异步 future）
     */
    std::future<LLMOutput> invoke_with_rendered_prompt(
        const RenderedPrompt& rendered,
        const std::string& provider = "",
        std::function<void(std::string_view)> stream_callback = nullptr
    );
    
    /**
     * @brief 配置模型参数
     * @param provider 提供商名称
     * @param config 模型配置
     */
    void configure(const std::string& provider, const ModelConfig& config);
    
    /**
     * @brief 获取所有已注册的适配器名称
     * @return 适配器名称列表
     */
    std::vector<std::string> list_providers() const;
    
    /**
     * @brief 获取模型名称（用于提示词渲染器选择格式化策略）
     * @param provider 提供商名称（空字符串表示使用默认）
     * @return 模型名称
     */
    std::string get_model_name(const std::string& provider = "") const;
    
private:
    std::shared_ptr<PromptRenderer> prompt_renderer_;  // 提示词渲染器
    std::map<std::string, std::shared_ptr<ModelAdapter>> adapters_;
    std::string default_provider_;
    std::mutex adapters_mutex_;
    std::mutex renderer_mutex_;
    
    /**
     * @brief 内部渲染提示词（如果未提供 RenderedPrompt）
     * @param input LLM 输入
     * @param provider 提供商名称
     * @return 渲染后的提示词
     */
    RenderedPrompt render_prompt(const LLMInput& input, const std::string& provider);
};

} // namespace agent_framework

#endif // __AGENT_LLM_CLIENT_H__
