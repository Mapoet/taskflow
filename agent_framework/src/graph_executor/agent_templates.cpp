/**
 * @file agent_templates.cpp
 * @brief Agent workflow templates (ReAct, etc.)
 */

#include <agent/graph_executor.hpp>

#include <stdexcept>

namespace agent_framework {

WorkflowResult WorkflowTemplate::execute(GraphExecutor& /*graph_executor*/,
                                         tf::Executor& /*executor*/,
                                         const ExecutionRequest& /*request*/) {
    WorkflowResult result;
    result.success = false;
    result.exit_code = 1;
    result.error_message = "workflow template does not support runtime execution: " +
                           get_template_name();
    return result;
}

void ReActTemplate::build(workflow::GraphBuilder& /*builder*/, const json& /*config*/) {
    throw std::logic_error(
        "ReActTemplate::build(json) cannot inject LLMClient/ToolBus. Use "
        "build_cli_agent_graph, GraphExecutor::build_agent_workflow, or ReActTemplate::build_react_loop.");
}

std::string ReActTemplate::get_template_name() const {
    return "react";
}

std::string ReActTemplate::get_template_description() const {
    return "ReAct-style agent loop (WP1.5)";
}

bool ReActTemplate::validate_config(const json& config) const {
    return config.is_object();
}

WorkflowResult ReActTemplate::execute(GraphExecutor& graph_executor,
                                      tf::Executor& executor,
                                      const ExecutionRequest& request) {
    ReactCliRunRequest react;
    react.config = request.config;
    react.deps = request.deps;
    react.session = request.session;
    react.options = request.options.react;
    return graph_executor.run_react_cli_sync(executor, react);
}

void ReActTemplate::build_react_loop(workflow::GraphBuilder& builder,
                                     const AgentConfig& config,
                                     const AgentWorkflowDeps& deps,
                                     std::shared_ptr<internal::AgentThreadState> agent_state,
                                     std::string_view loop_node_name,
                                     const CliAgentGraphOptions& graph_options) {
    build_cli_agent_graph(builder, config, deps, std::move(agent_state), loop_node_name,
                          graph_options);
}

void BatchToolCallTemplate::build(workflow::GraphBuilder& builder, const json& config) {
    (void)builder;
    (void)config;
}

std::string BatchToolCallTemplate::get_template_name() const {
    return "batch_tool";
}

std::string BatchToolCallTemplate::get_template_description() const {
    return "Parallel tool calls";
}

bool BatchToolCallTemplate::validate_config(const json& config) const {
    return config.is_object();
}

void BatchToolCallTemplate::build_parallel_tool_calls(workflow::GraphBuilder& builder,
                                                      const json& config) {
    (void)builder;
    (void)config;
}

void MultimodalRAGTemplate::build(workflow::GraphBuilder& builder, const json& config) {
    (void)builder;
    (void)config;
}

std::string MultimodalRAGTemplate::get_template_name() const {
    return "multimodal_rag";
}

std::string MultimodalRAGTemplate::get_template_description() const {
    return "Multimodal RAG";
}

bool MultimodalRAGTemplate::validate_config(const json& config) const {
    return config.is_object();
}

void MultimodalRAGTemplate::build_multimodal_retrieval(workflow::GraphBuilder& builder,
                                                        const json& config) {
    (void)builder;
    (void)config;
}

} // namespace agent_framework
