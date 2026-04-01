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
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace agent_framework {

class LLMClient;
class ToolBus;

namespace internal {
struct AgentThreadState;
}

/**
 * @brief WP1.5 ReAct Agent 图所需的运行时依赖（LLM + ToolBus）
 *
 * PromptRenderer 由调用方在 LLMClient 上 `set_prompt_renderer` 配置，不重复放入此结构。
 */
struct AgentWorkflowDeps {
    std::shared_ptr<LLMClient> llm;
    std::shared_ptr<ToolBus> toolbus;
};

/**
 * @brief 可选构图参数（WP1.6 流式等）
 */
struct CliAgentGraphOptions {
    std::function<void(std::string_view)> stream_callback{};
};

/**
 * @brief 构建与 test_agent_loop_wp5 等价的 CLI ReAct 图：内置 SystemPrompt / UserInput / AgentState 源 + AgentLoop
 *
 * 节点契约（与 WP1.5 一致）：源名 `SystemPrompt`/`UserInput`/`AgentState`；输出键见 internal::loop_io_keys.hpp。
 * @param loop_node_name Loop 节点名，默认 `AgentLoop`
 *
 * 注意：传字面量 `nullptr` 会与 string_view 重载产生歧义，请使用
 * `static_cast<std::shared_ptr<internal::AgentThreadState>>(nullptr)` 或具名变量。
 */
void build_cli_agent_graph(
    workflow::GraphBuilder& builder,
    const AgentConfig& config,
    const AgentWorkflowDeps& deps,
    std::shared_ptr<internal::AgentThreadState> agent_state,
    std::string_view loop_node_name = "AgentLoop",
    const CliAgentGraphOptions& graph_options = CliAgentGraphOptions());

/**
 * @brief 便捷重载：内部创建 AgentThreadState 并设置 initial_user_prompt
 */
void build_cli_agent_graph(
    workflow::GraphBuilder& builder,
    const AgentConfig& config,
    const AgentWorkflowDeps& deps,
    std::string_view user_query,
    std::string_view loop_node_name = "AgentLoop",
    const CliAgentGraphOptions& graph_options = CliAgentGraphOptions());

/**
 * @brief 可选终端 Sink：Loop 退出后将结构化终稿交给单一回调（与 WP1.6 `handle_final_result` 字段对齐）
 *
 * `on_final_json` 必填；为空则 `build_cli_agent_graph_with_terminal_sink` 抛 `std::invalid_argument`。
 * 回调收到的 JSON：`final_answer`（string）、`iteration`（number）、`history_size`（number）。
 *
 * **去重**：若 LLM 已配置 `stream_callback` 向用户增量打印全文，请勿在回调中再次全文打印终稿；约定仅一处负责用户可见终稿。
 *
 * `on_final_state`（可选）：与 `on_final_json` 同次 Sink 调度内调用，参数为 Loop 输出的 `next_agent_state`
 *（可能与调用方传入的 `agent_state` 非同一 `shared_ptr`，因循环内会更新状态）。用于需要读 `history` 的消费方或测试。
 */
struct CliAgentTerminalSinkOptions {
    std::string sink_node_name = "CliOutputSink";
    std::function<void(const json&)> on_final_json;
    std::function<void(const std::shared_ptr<internal::AgentThreadState>&)> on_final_state;
};

/**
 * @brief 在 `build_cli_agent_graph` 基础上追加 `create_any_sink`，订阅 Loop 的 `final_answer` / `next_agent_state`
 * @param loop_node_name 与构图时 Loop 节点名一致，默认 `AgentLoop`
 */
void build_cli_agent_graph_with_terminal_sink(
    workflow::GraphBuilder& builder,
    const AgentConfig& config,
    const AgentWorkflowDeps& deps,
    std::shared_ptr<internal::AgentThreadState> agent_state,
    const CliAgentTerminalSinkOptions& sink,
    std::string_view loop_node_name = "AgentLoop",
    const CliAgentGraphOptions& graph_options = CliAgentGraphOptions());

/**
 * @brief 便捷重载：内部创建 `AgentThreadState` 并设置 `initial_user_prompt`
 */
void build_cli_agent_graph_with_terminal_sink(
    workflow::GraphBuilder& builder,
    const AgentConfig& config,
    const AgentWorkflowDeps& deps,
    std::string_view user_query,
    const CliAgentTerminalSinkOptions& sink,
    std::string_view loop_node_name = "AgentLoop",
    const CliAgentGraphOptions& graph_options = CliAgentGraphOptions());

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
 *
 * WorkflowTemplate::build(json) 无法提供 LLM/ToolBus shared_ptr，将抛异常；请使用 build_react_loop 或
 * build_cli_agent_graph / GraphExecutor::build_agent_workflow。
 */
class ReActTemplate : public WorkflowTemplate {
public:
    void build(workflow::GraphBuilder& builder, const json& config) override;
    std::string get_template_name() const override;
    std::string get_template_description() const override;
    bool validate_config(const json& config) const override;

    /**
     * @brief 与 build_cli_agent_graph 等价，便于以模板类名义调用
     */
    static void build_react_loop(
        workflow::GraphBuilder& builder,
        const AgentConfig& config,
        const AgentWorkflowDeps& deps,
        std::shared_ptr<internal::AgentThreadState> agent_state,
        std::string_view loop_node_name = "AgentLoop",
        const CliAgentGraphOptions& graph_options = CliAgentGraphOptions());
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
     * @brief 构建标准 Agent（ReAct）工作流
     * @param deps LLM 与 ToolBus；renderer 请在 llm 上预配置
     * @param agent_state 非空；initial_user_prompt 为首轮用户内容
     */
    void build_agent_workflow(const AgentConfig& config,
                              workflow::GraphBuilder& builder,
                              const AgentWorkflowDeps& deps,
                              std::shared_ptr<internal::AgentThreadState> agent_state);

    /**
     * @brief 同 `build_agent_workflow`，并追加终端 Sink（见 `CliAgentTerminalSinkOptions`）
     */
    void build_agent_workflow(const AgentConfig& config,
                              workflow::GraphBuilder& builder,
                              const AgentWorkflowDeps& deps,
                              std::shared_ptr<internal::AgentThreadState> agent_state,
                              const CliAgentTerminalSinkOptions& sink);

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
    mutable std::mutex templates_mutex_;
    mutable std::mutex workflows_mutex_;
};

} // namespace agent_framework

#endif // __AGENT_GRAPH_EXECUTOR_H__
