/**
 * @file graph_executor.cpp
 * @brief GraphExecutor implementation
 */

#include <agent/graph_executor.hpp>

#include <future>
#include <stdexcept>

namespace agent_framework {

void GraphExecutor::build_agent_workflow(const AgentConfig& config,
                                        workflow::GraphBuilder& builder,
                                        const AgentWorkflowDeps& deps,
                                        std::shared_ptr<internal::AgentThreadState> agent_state) {
    build_cli_agent_graph(builder, config, deps, std::move(agent_state));
}

void GraphExecutor::build_agent_workflow(const AgentConfig& config,
                                        workflow::GraphBuilder& builder,
                                        const AgentWorkflowDeps& deps,
                                        std::shared_ptr<internal::AgentThreadState> agent_state,
                                        const CliAgentTerminalSinkOptions& sink) {
    build_cli_agent_graph_with_terminal_sink(builder, config, deps, std::move(agent_state), sink);
}

void GraphExecutor::build_custom_workflow(const WorkflowConfig& /*config*/,
                                          workflow::GraphBuilder& /*builder*/) {
    // WP1.6+: 自定义节点编排
    throw std::logic_error("GraphExecutor::build_custom_workflow: not implemented");
}

void GraphExecutor::register_template(const std::string& name,
                                      std::shared_ptr<WorkflowTemplate> template_ptr) {
    if (!template_ptr) {
        throw std::invalid_argument("GraphExecutor::register_template: null template");
    }
    std::lock_guard<std::mutex> lock(templates_mutex_);
    templates_[name] = std::move(template_ptr);
}

std::future<WorkflowResult> GraphExecutor::execute(const std::string& workflow_name) {
    // WP1.6+: 持有 GraphBuilder + Executor 的运行管线
    return std::async(std::launch::deferred, [workflow_name]() -> WorkflowResult {
        (void)workflow_name;
        throw std::logic_error("GraphExecutor::execute: not implemented");
    });
}

std::shared_ptr<WorkflowTemplate> GraphExecutor::get_template(const std::string& name) const {
    std::lock_guard<std::mutex> lock(templates_mutex_);
    auto it = templates_.find(name);
    if (it == templates_.end()) {
        return nullptr;
    }
    return it->second;
}

std::vector<std::string> GraphExecutor::list_templates() const {
    std::lock_guard<std::mutex> lock(templates_mutex_);
    std::vector<std::string> out;
    out.reserve(templates_.size());
    for (const auto& kv : templates_) {
        out.push_back(kv.first);
    }
    return out;
}

} // namespace agent_framework
