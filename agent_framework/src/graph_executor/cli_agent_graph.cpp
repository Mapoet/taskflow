/**
 * @file cli_agent_graph.cpp
 * @brief build_cli_agent_graph — WP1.5 ReAct 构图工厂
 */

#include <agent/graph_executor.hpp>

#include <agent/internal/agent_thread_state.hpp>
#include <agent/internal/loop_io_keys.hpp>
#include <agent/llm_client.hpp>
#include <agent/toolbus.hpp>
#include <node/agent_loop_node.hpp>

#include <stdexcept>
#include <string>
#include <unordered_map>

namespace agent_framework {

namespace {

void build_cli_agent_graph_impl(workflow::GraphBuilder& builder,
                                const AgentConfig& config,
                                const AgentWorkflowDeps& deps,
                                const std::shared_ptr<internal::AgentThreadState>& agent_state,
                                std::string_view loop_node_name) {
    if (!deps.llm) {
        throw std::invalid_argument("build_cli_agent_graph: deps.llm is null");
    }
    if (!deps.toolbus) {
        throw std::invalid_argument("build_cli_agent_graph: deps.toolbus is null");
    }
    if (!agent_state) {
        throw std::invalid_argument("build_cli_agent_graph: agent_state is null");
    }

    auto [sys_src, _st] = builder.create_any_source(
        "SystemPrompt",
        std::unordered_map<std::string, std::any>{
            {std::string(internal::kSystemPrompt), std::any{config.system_prompt}}});
    (void)sys_src;

    auto [user_src, _ut] = builder.create_any_source(
        "UserInput",
        std::unordered_map<std::string, std::any>{
            {std::string(internal::kUserQuery), std::any{agent_state->initial_user_prompt}}});
    (void)user_src;

    auto [state_src, _at] = builder.create_any_source(
        "AgentState",
        std::unordered_map<std::string, std::any>{
            {std::string(internal::kAgentState), std::any{agent_state}}});
    (void)state_src;

    const std::string loop_name{loop_node_name};
    auto [loop_node, loop_task] = node::AgentLoopNode::create(
        builder,
        loop_name,
        config,
        deps.llm,
        deps.toolbus,
        nullptr,
        nullptr,
        {{"SystemPrompt", std::string(internal::kSystemPrompt)},
         {"UserInput", std::string(internal::kUserQuery)},
         {"AgentState", std::string(internal::kAgentState)}},
        {std::string(internal::kFinalAnswer), std::string(internal::kNextAgentState),
         std::string(internal::kLlmOutput)});
    (void)loop_node;
    (void)loop_task;
}

} // namespace

void build_cli_agent_graph(workflow::GraphBuilder& builder,
                           const AgentConfig& config,
                           const AgentWorkflowDeps& deps,
                           std::shared_ptr<internal::AgentThreadState> agent_state,
                           std::string_view loop_node_name) {
    build_cli_agent_graph_impl(builder, config, deps, agent_state, loop_node_name);
}

void build_cli_agent_graph(workflow::GraphBuilder& builder,
                           const AgentConfig& config,
                           const AgentWorkflowDeps& deps,
                           std::string_view user_query,
                           std::string_view loop_node_name) {
    auto st = std::make_shared<internal::AgentThreadState>();
    st->initial_user_prompt = std::string(user_query);
    build_cli_agent_graph_impl(builder, config, deps, st, loop_node_name);
}

} // namespace agent_framework
