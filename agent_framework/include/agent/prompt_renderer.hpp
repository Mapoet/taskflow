/**
 * @file prompt_renderer.hpp
 * @brief 提示词渲染模块：将 LLMInput 转换为 RenderedPrompt
 * @author Mapoet
 * @version 0.1
 * @date 2025-01-XX
 */
#ifndef __AGENT_PROMPT_RENDERER_H__
#define __AGENT_PROMPT_RENDERER_H__

#include "types.hpp"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <regex>
#include <mutex>

namespace agent_framework {

// ============================================================================
// 提示词模板接口
// ============================================================================

/**
 * @brief 提示词模板虚基类
 * 定义统一的模板渲染接口
 */
class PromptTemplate {
public:
    virtual ~PromptTemplate() = default;
    
    /**
     * @brief 渲染模板，替换所有变量
     * @param variables 变量映射（变量名 -> 变量值）
     * @return 渲染后的文本
     */
    virtual std::string render(const std::map<std::string, std::string>& variables) = 0;
    
    /**
     * @brief 加载模板（从文件或字符串）
     * @param source 模板源（文件路径或模板字符串）
     */
    virtual void load(const std::string& source) = 0;
    
    /**
     * @brief 获取模板中使用的变量列表
     * @return 变量名列表
     */
    virtual std::vector<std::string> get_variables() const = 0;
    
    /**
     * @brief 验证变量是否完整
     * @param variables 变量映射
     * @return true 如果所有必需变量都存在
     */
    virtual bool validate_variables(const std::map<std::string, std::string>& variables) const = 0;
};

/**
 * @brief 字符串模板实现（支持 {{variable}} 占位符）
 */
class StringPromptTemplate : public PromptTemplate {
public:
    explicit StringPromptTemplate(const std::string& template_str);
    
    std::string render(const std::map<std::string, std::string>& variables) override;
    void load(const std::string& source) override;
    std::vector<std::string> get_variables() const override;
    bool validate_variables(const std::map<std::string, std::string>& variables) const override;
    
private:
    std::string template_str_;
    std::regex var_pattern_;  // 匹配 {{variable}}
    
    /**
     * @brief 提取模板中的所有变量名
     * @return 变量名列表
     */
    std::vector<std::string> extract_variables() const;
};

/**
 * @brief 文件模板实现（从文件加载模板）
 */
class FilePromptTemplate : public PromptTemplate {
public:
    explicit FilePromptTemplate(const std::string& file_path);
    
    std::string render(const std::map<std::string, std::string>& variables) override;
    void load(const std::string& source) override;
    std::vector<std::string> get_variables() const override;
    bool validate_variables(const std::map<std::string, std::string>& variables) const override;
    
private:
    std::string file_path_;
    std::shared_ptr<StringPromptTemplate> inner_template_;
};

// ============================================================================
// 工具格式化器接口
// ============================================================================

/**
 * @brief 工具格式化器虚基类
 * 定义统一的工具列表格式化接口，支持不同 LLM 提供商的格式
 */
class ToolFormatter {
public:
    virtual ~ToolFormatter() = default;
    
    /**
     * @brief 格式化为 JSON（供 API 使用）
     * @param tools 工具元数据列表
     * @return JSON 格式的工具列表
     */
    virtual json format_tools(const std::vector<ToolMeta>& tools) = 0;
    
    /**
     * @brief 格式化为文本（供模板使用）
     * @param tools 工具元数据列表
     * @return 文本格式的工具描述
     */
    virtual std::string format_tools_as_text(const std::vector<ToolMeta>& tools) = 0;
    
    /**
     * @brief 获取格式化器支持的模型列表
     * @return 支持的模型名称列表（支持通配符匹配）
     */
    virtual std::vector<std::string> supported_models() const = 0;
};

/**
 * @brief OpenAI 工具格式化器
 */
class OpenAIToolFormatter : public ToolFormatter {
public:
    json format_tools(const std::vector<ToolMeta>& tools) override;
    std::string format_tools_as_text(const std::vector<ToolMeta>& tools) override;
    std::vector<std::string> supported_models() const override;
    
private:
    /**
     * @brief 将 ToolMeta 转换为 OpenAI Function Calling 格式
     * @param tool 工具元数据
     * @return JSON 格式的函数定义
     */
    json convert_to_openai_format(const ToolMeta& tool);
};

/**
 * @brief Anthropic 工具格式化器
 */
class AnthropicToolFormatter : public ToolFormatter {
public:
    json format_tools(const std::vector<ToolMeta>& tools) override;
    std::string format_tools_as_text(const std::vector<ToolMeta>& tools) override;
    std::vector<std::string> supported_models() const override;
    
private:
    /**
     * @brief 将 ToolMeta 转换为 Anthropic Tool Use 格式
     * @param tool 工具元数据
     * @return JSON 格式的工具定义
     */
    json convert_to_anthropic_format(const ToolMeta& tool);
};

/**
 * @brief Gemini 工具格式化器
 */
class GeminiToolFormatter : public ToolFormatter {
public:
    json format_tools(const std::vector<ToolMeta>& tools) override;
    std::string format_tools_as_text(const std::vector<ToolMeta>& tools) override;
    std::vector<std::string> supported_models() const override;
    
private:
    /**
     * @brief 将 ToolMeta 转换为 Gemini Function Calling 格式
     * @param tool 工具元数据
     * @return JSON 格式的函数定义
     */
    json convert_to_gemini_format(const ToolMeta& tool);
};

// ============================================================================
// 对话历史格式化器接口
// ============================================================================

/**
 * @brief 对话历史格式化器虚基类
 * 定义统一的对话历史格式化接口
 */
class HistoryFormatter {
public:
    virtual ~HistoryFormatter() = default;
    
    /**
     * @brief 格式化为文本（供模板使用）
     * @param history 对话历史
     * @return 文本格式的对话历史
     */
    virtual std::string format_as_text(const std::vector<Message>& history) = 0;
    
    /**
     * @brief 格式化为消息列表（供 API 使用）
     * @param history 对话历史
     * @return JSON 数组格式的消息列表（OpenAI messages 格式）
     */
    virtual std::vector<json> format_as_messages(const std::vector<Message>& history) = 0;
    
    /**
     * @brief 截断历史（保留最近的 N 轮）
     * @param history 原始对话历史
     * @param max_messages 最大消息数
     * @return 截断后的对话历史
     */
    virtual std::vector<Message> truncate(const std::vector<Message>& history, int max_messages) = 0;
};

/**
 * @brief OpenAI 历史格式化器（messages 格式）
 */
class OpenAIHistoryFormatter : public HistoryFormatter {
public:
    std::string format_as_text(const std::vector<Message>& history) override;
    std::vector<json> format_as_messages(const std::vector<Message>& history) override;
    std::vector<Message> truncate(const std::vector<Message>& history, int max_messages) override;
};

// ============================================================================
// 提示词渲染器（核心类）
// ============================================================================

/**
 * @brief 提示词渲染器（核心类）
 * 负责将 LLMInput 渲染成最终的提示词
 */
class PromptRenderer {
public:
    explicit PromptRenderer(std::shared_ptr<PromptTemplate> template_ptr);
    
    /**
     * @brief 渲染提示词（主要接口）
     * @param input LLM 输入（包含所有输入源）
     * @param model_name 模型名称（用于选择格式化策略）
     * @return 渲染后的提示词
     */
    RenderedPrompt render(const LLMInput& input, const std::string& model_name);
    
    /**
     * @brief 注册工具格式化器
     * @param model_pattern 模型名称匹配模式（支持通配符，如 "gpt-*", "claude-*"）
     * @param formatter 工具格式化器
     */
    void register_tool_formatter(const std::string& model_pattern, 
                                std::shared_ptr<ToolFormatter> formatter);
    
    /**
     * @brief 设置历史格式化器
     * @param formatter 历史格式化器
     */
    void set_history_formatter(std::shared_ptr<HistoryFormatter> formatter);
    
    /**
     * @brief 设置提示词模板
     * @param template_ptr 提示词模板
     */
    void set_template(std::shared_ptr<PromptTemplate> template_ptr);
    
    /**
     * @brief 配置上下文窗口限制
     * @param model_name 模型名称
     * @param max_tokens 最大 token 数
     */
    void set_max_tokens(const std::string& model_name, int max_tokens);
    
private:
    std::shared_ptr<PromptTemplate> template_;
    std::map<std::string, std::shared_ptr<ToolFormatter>> tool_formatters_;
    std::shared_ptr<HistoryFormatter> history_formatter_;
    std::map<std::string, int> max_tokens_map_;
    std::mutex formatters_mutex_;
    
    /**
     * @brief 获取工具格式化器（根据模型名称匹配）
     * @param model_name 模型名称
     * @return 匹配的工具格式化器，如果未找到则返回 nullptr
     */
    std::shared_ptr<ToolFormatter> get_tool_formatter(const std::string& model_name);
    
    /**
     * @brief 估算 token 数
     * @param rendered 渲染后的提示词
     * @return 估算的 token 数
     */
    int estimate_tokens(const RenderedPrompt& rendered);
    
    /**
     * @brief 截断提示词（保留优先级高的内容）
     * @param rendered 原始渲染结果
     * @param model_name 模型名称
     * @return 截断后的提示词
     */
    RenderedPrompt truncate_prompt(const RenderedPrompt& rendered, const std::string& model_name);
    
    /**
     * @brief 整合多模态输入到消息列表
     * @param rendered 渲染结果（将被修改）
     * @param input LLM 输入
     */
    void integrate_multimodal_input(RenderedPrompt& rendered, const LLMInput& input);
};

} // namespace agent_framework

#endif // __AGENT_PROMPT_RENDERER_H__

