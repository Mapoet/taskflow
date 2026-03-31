/**
 * @file agent_templates.cpp
 * @brief Agent workflow templates (ReAct, etc.)
 */

#include "agent/graph_executor.hpp"
#include "node/agent_loop_node.hpp"

namespace agent_framework {

void ReActTemplate::build(workflow::GraphBuilder& builder, const json& config) {
    (void)config;
    AgentConfig cfg;
    build_react_loop(builder, cfg);
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

void ReActTemplate::build_react_loop(workflow::GraphBuilder& builder, const AgentConfig& config) {
    // Template-level builder: only wires the loop node placeholder.
    // Concrete injection of llm_client/toolbus happens in higher-level GraphExecutor usage.
    // This function is intentionally minimal for WP1.5.
    (void)builder;
    (void)config;
}

} // namespace agent_framework

