/**
 * @file graph_executor.hpp
 * @brief GraphExecutor 模块：工作流构建和执行
 * @author Mapoet
 * @version 0.1
 * @date 2025-01-XX
 */
#ifndef __AGENT_GRAPH_EXECUTOR_H__
#define __AGENT_GRAPH_EXECUTOR_H__

#include "types.hpp"
#include <workflow/nodeflow.hpp>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <future>
#include <mutex>

namespace agent_framework {

// ============================================================================
// 工作流模板接口
// ============================================================================

/**
 * @brief 工作流模板构建器虚基类
 * 定义统一的工作流构建接口
 */
class WorkflowTemplate {
public:
    virtual ~WorkflowTemplate() = default;
    
    /**
     * @brief 构建工作流
     * @param builder 图构建器
     * @param config 配置（JSON 格式）
     */
    virtual void build(workflow::GraphBuilder& builder, const json& config) = 0;
    
    /**
     * @brief 获取模板名称
     * @return 模板名称
     */
    virtual std::string get_template_name() const = 0;
    
    /**
     * @brief 获取模板描述
     * @return 模板描述
     */
    virtual std::string get_template_description() const = 0;
    
    /**
     * @brief 验证配置
     * @param config 配置（JSON 格式）
     * @return true 如果配置有效
     */
    virtual bool validate_config(const json& config) const = 0;
};

/**
 * @brief ReAct 循环模板
 */
class ReActTemplate : public WorkflowTemplate {
public:
    void build(workflow::GraphBuilder& builder, const json& config) override;
    std::string get_template_name() const override;
    std::string get_template_description() const override;
    bool validate_config(const json& config) const override;
    
private:
    /**
     * @brief 构建 ReAct 循环体
     * @param builder 图构建器
     * @param config Agent 配置
     */
    void build_react_loop(workflow::GraphBuilder& builder, const AgentConfig& config);
};

/**
 * @brief 批量工具调用模板
 */
class BatchToolCallTemplate : public WorkflowTemplate {
public:
    void build(workflow::GraphBuilder& builder, const json& config) override;
    std::string get_template_name() const override;
    std::string get_template_description() const override;
    bool validate_config(const json& config) const override;
    
private:
    /**
     * @brief 构建并行工具调用节点
     * @param builder 图构建器
     * @param config 配置
     */
    void build_parallel_tool_calls(workflow::GraphBuilder& builder, const json& config);
};

/**
 * @brief 多模态 RAG 模板
 */
class MultimodalRAGTemplate : public WorkflowTemplate {
public:
    void build(workflow::GraphBuilder& builder, const json& config) override;
    std::string get_template_name() const override;
    std::string get_template_description() const override;
    bool validate_config(const json& config) const override;
    
private:
    /**
     * @brief 构建多模态检索流程
     * @param builder 图构建器
     * @param config 配置
     */
    void build_multimodal_retrieval(workflow::GraphBuilder& builder, const json& config);
};

// ============================================================================
// GraphExecutor 管理器
// ============================================================================

/**
 * @brief GraphExecutor 管理器
 */
class GraphExecutor {
public:
    /**
     * @brief 构建标准 Agent 工作流
     * @param config Agent 配置
     * @param builder 图构建器
     */
    void build_agent_workflow(const AgentConfig& config, workflow::GraphBuilder& builder);
    
    /**
     * @brief 构建自定义工作流
     * @param config 工作流配置
     * @param builder 图构建器
     */
    void build_custom_workflow(const WorkflowConfig& config, workflow::GraphBuilder& builder);
    
    /**
     * @brief 注册工作流模板
     * @param name 模板名称
     * @param template_ptr 模板指针
     */
    void register_template(const std::string& name, 
                          std::shared_ptr<WorkflowTemplate> template_ptr);
    
    /**
     * @brief 执行工作流
     * @param workflow_name 工作流名称
     * @return 工作流执行结果（异步 future）
     */
    std::future<WorkflowResult> execute(const std::string& workflow_name);
    
    /**
     * @brief 获取模板
     * @param name 模板名称
     * @return 模板指针（如果存在）
     */
    std::shared_ptr<WorkflowTemplate> get_template(const std::string& name) const;
    
    /**
     * @brief 列出所有模板
     * @return 模板名称列表
     */
    std::vector<std::string> list_templates() const;
    
private:
    std::map<std::string, std::shared_ptr<WorkflowTemplate>> templates_;
    std::map<std::string, workflow::GraphBuilder> workflows_;
    std::mutex templates_mutex_;
    std::mutex workflows_mutex_;
    
    /**
     * @brief 构建默认 Agent 工作流（使用 ReAct 模板）
     * @param config Agent 配置
     * @param builder 图构建器
     */
    void build_default_agent_workflow(const AgentConfig& config, workflow::GraphBuilder& builder);
};

} // namespace agent_framework

#endif // __AGENT_GRAPH_EXECUTOR_H__
