/**
 * @file cli_agent_graph.cpp
 * @brief build_cli_agent_graph — WP1.5 ReAct 构图工厂
 */

#include <agent/graph_executor/graph_executor.hpp>

#include <agent/internal/agent_thread_state.hpp>
#include <agent/internal/loop_io_keys.hpp>
#include <agent/llm_client/llm_client.hpp>
#include <agent/toolbus/draw_tools.hpp>
#include <agent/toolbus/expr_tools.hpp>
#include <agent/toolbus/fs_tools.hpp>
#include <agent/toolbus/news_sources_tool.hpp>
#include <agent/toolbus/web_tools.hpp>
#include <agent/skills/skill_script_tool.hpp>
#include <agent/toolbus/toolbus.hpp>
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
                                std::string_view loop_node_name,
                                const CliAgentGraphOptions& graph_options) {
    if (!deps.llm) {
        throw std::invalid_argument("build_cli_agent_graph: deps.llm is null");
    }
    if (!deps.toolbus) {
        throw std::invalid_argument("build_cli_agent_graph: deps.toolbus is null");
    }
    if (!agent_state) {
        throw std::invalid_argument("build_cli_agent_graph: agent_state is null");
    }

    if (deps.skills) {
        register_skill_script_tool(*deps.toolbus, deps.skills);
    }
    deps.toolbus->ensure_default_tools_registered([&] {
        register_builtin_fs_tools_if_configured(*deps.toolbus);
        register_builtin_web_tools_if_configured(*deps.toolbus);
        register_web_configured_source_if_configured(*deps.toolbus);
        register_builtin_expr_tools_if_configured(*deps.toolbus);
        register_builtin_draw_tools_if_configured(*deps.toolbus);
    });

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
         std::string(internal::kLlmOutput)},
        graph_options.stream_callback,
        deps.skills,
        graph_options.task_control,
        graph_options.tool_execution_observer,
        graph_options.skill_event_sink);
    (void)loop_node;
    (void)loop_task;
}

void append_cli_terminal_sink(workflow::GraphBuilder& builder,
                              std::string_view loop_node_name,
                              const CliAgentTerminalSinkOptions& sink) {
    if (!sink.on_final_json) {
        throw std::invalid_argument(
            "build_cli_agent_graph_with_terminal_sink: CliAgentTerminalSinkOptions::on_final_json is empty");
    }
    const std::string loop_name{loop_node_name};
    std::string sink_name = sink.sink_node_name;
    if (sink_name.empty()) {
        sink_name = "CliOutputSink";
    }
    std::function<void(const json&)> cb = sink.on_final_json;
    std::function<void(const std::shared_ptr<internal::AgentThreadState>&)> cb_state =
        sink.on_final_state;
    auto [sn, st] = builder.create_any_sink(
        sink_name,
        {{loop_name, std::string(internal::kFinalAnswer)},
         {loop_name, std::string(internal::kNextAgentState)}},
        [cb = std::move(cb), cb_state = std::move(cb_state)](
            const std::unordered_map<std::string, std::any>& outs) {
            const std::string final_answer =
                std::any_cast<std::string>(outs.at(std::string(internal::kFinalAnswer)));
            auto st_ptr = std::any_cast<std::shared_ptr<internal::AgentThreadState>>(
                outs.at(std::string(internal::kNextAgentState)));
            json j;
            j["final_answer"] = final_answer;
            j["iteration"] = st_ptr ? st_ptr->iteration : 0;
            j["history_size"] = st_ptr ? static_cast<std::size_t>(st_ptr->history.size()) : 0;
            // Optional guard fields (best-effort; keep backward compatibility)
            // Convention: guard-triggered final_answer starts with "[guard]".
            bool guard_triggered = final_answer.rfind("[guard]", 0) == 0;
            j["guard_triggered"] = guard_triggered;
            if (guard_triggered) {
                // Parse reason=... token if present.
                std::string reason;
                const std::string k = "reason=";
                const std::size_t pos = final_answer.find(k);
                if (pos != std::string::npos) {
                    const std::size_t start = pos + k.size();
                    std::size_t end = final_answer.find_first_of(" \n\r\t", start);
                    if (end == std::string::npos) {
                        end = final_answer.size();
                    }
                    reason = final_answer.substr(start, end - start);
                }
                j["guard_reason"] = reason;
                j["guard_details"] = final_answer;
            } else {
                j["guard_reason"] = "";
                j["guard_details"] = "";
            }
            cb(j);
            if (cb_state) {
                cb_state(st_ptr);
            }
        });
    (void)sn;
    (void)st;
}

} // namespace

void build_cli_agent_graph(workflow::GraphBuilder& builder,
                           const AgentConfig& config,
                           const AgentWorkflowDeps& deps,
                           std::shared_ptr<internal::AgentThreadState> agent_state,
                           std::string_view loop_node_name,
                           const CliAgentGraphOptions& graph_options) {
    build_cli_agent_graph_impl(builder, config, deps, agent_state, loop_node_name, graph_options);
}

void build_cli_agent_graph(workflow::GraphBuilder& builder,
                           const AgentConfig& config,
                           const AgentWorkflowDeps& deps,
                           std::string_view user_query,
                           std::string_view loop_node_name,
                           const CliAgentGraphOptions& graph_options) {
    auto st = std::make_shared<internal::AgentThreadState>();
    st->initial_user_prompt = std::string(user_query);
    build_cli_agent_graph_impl(builder, config, deps, st, loop_node_name, graph_options);
}

void build_cli_agent_graph_with_terminal_sink(
    workflow::GraphBuilder& builder,
    const AgentConfig& config,
    const AgentWorkflowDeps& deps,
    std::shared_ptr<internal::AgentThreadState> agent_state,
    const CliAgentTerminalSinkOptions& sink,
    std::string_view loop_node_name,
    const CliAgentGraphOptions& graph_options) {
    build_cli_agent_graph_impl(builder, config, deps, agent_state, loop_node_name, graph_options);
    append_cli_terminal_sink(builder, loop_node_name, sink);
}

void build_cli_agent_graph_with_terminal_sink(
    workflow::GraphBuilder& builder,
    const AgentConfig& config,
    const AgentWorkflowDeps& deps,
    std::string_view user_query,
    const CliAgentTerminalSinkOptions& sink,
    std::string_view loop_node_name,
    const CliAgentGraphOptions& graph_options) {
    auto st = std::make_shared<internal::AgentThreadState>();
    st->initial_user_prompt = std::string(user_query);
    build_cli_agent_graph_with_terminal_sink(builder, config, deps, std::move(st), sink,
                                              loop_node_name, graph_options);
}

} // namespace agent_framework
