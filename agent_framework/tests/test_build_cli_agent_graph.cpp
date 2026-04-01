/**
 * @file test_build_cli_agent_graph.cpp
 * @brief build_cli_agent_graph / GraphExecutor::build_agent_workflow 构图契约（无 Live LLM）
 */

#include <agent/graph_executor.hpp>
#include <agent/internal/agent_thread_state.hpp>
#include <agent/llm_client.hpp>
#include <agent/toolbus.hpp>
#include <agent/types.hpp>

#include <cassert>
#include <iostream>
#include <stdexcept>

namespace {

using namespace agent_framework;

void test_null_llm_throws() {
    workflow::GraphBuilder b("t1");
    AgentConfig cfg;
    cfg.system_prompt = "sys";
    AgentWorkflowDeps deps;
    deps.llm = nullptr;
    deps.toolbus = std::make_shared<ToolBus>();
    auto st = std::make_shared<internal::AgentThreadState>();
    st->initial_user_prompt = "u";
    try {
        build_cli_agent_graph(b, cfg, deps, st);
        assert(false);
    } catch (const std::invalid_argument&) {
    }
}

void test_null_toolbus_throws() {
    workflow::GraphBuilder b("t2");
    AgentConfig cfg;
    cfg.system_prompt = "sys";
    AgentWorkflowDeps deps;
    deps.llm = std::make_shared<LLMClient>();
    deps.toolbus = nullptr;
    auto st = std::make_shared<internal::AgentThreadState>();
    st->initial_user_prompt = "u";
    try {
        build_cli_agent_graph(b, cfg, deps, st);
        assert(false);
    } catch (const std::invalid_argument&) {
    }
}

void test_null_state_throws() {
    workflow::GraphBuilder b("t3");
    AgentConfig cfg;
    cfg.system_prompt = "sys";
    AgentWorkflowDeps deps;
    deps.llm = std::make_shared<LLMClient>();
    deps.toolbus = std::make_shared<ToolBus>();
    try {
        build_cli_agent_graph(
            b, cfg, deps, static_cast<std::shared_ptr<internal::AgentThreadState>>(nullptr));
        assert(false);
    } catch (const std::invalid_argument&) {
    }
}

void test_build_succeeds_minimal() {
    workflow::GraphBuilder b("t4");
    AgentConfig cfg;
    cfg.system_prompt = "sys";
    AgentWorkflowDeps deps;
    deps.llm = std::make_shared<LLMClient>();
    deps.toolbus = std::make_shared<ToolBus>();
    auto st = std::make_shared<internal::AgentThreadState>();
    st->initial_user_prompt = "hello";
    build_cli_agent_graph(b, cfg, deps, st);

    GraphExecutor ex;
    workflow::GraphBuilder b2("t5");
    ex.build_agent_workflow(cfg, b2, deps, st);
}

void test_user_query_overload() {
    workflow::GraphBuilder b("t6");
    AgentConfig cfg;
    cfg.system_prompt = "sys";
    AgentWorkflowDeps deps;
    deps.llm = std::make_shared<LLMClient>();
    deps.toolbus = std::make_shared<ToolBus>();
    build_cli_agent_graph(b, cfg, deps, "q");
}

void test_react_template_build_throws() {
    ReActTemplate tmpl;
    workflow::GraphBuilder b("t7");
    try {
        tmpl.build(b, json::object());
        assert(false);
    } catch (const std::logic_error&) {
    }
}

void test_terminal_sink_empty_callback_throws() {
    workflow::GraphBuilder b("t8");
    AgentConfig cfg;
    cfg.system_prompt = "sys";
    AgentWorkflowDeps deps;
    deps.llm = std::make_shared<LLMClient>();
    deps.toolbus = std::make_shared<ToolBus>();
    auto st = std::make_shared<internal::AgentThreadState>();
    st->initial_user_prompt = "u";
    CliAgentTerminalSinkOptions sink;
    sink.on_final_json = nullptr;
    try {
        build_cli_agent_graph_with_terminal_sink(b, cfg, deps, st, sink);
        assert(false);
    } catch (const std::invalid_argument&) {
    }
}

void test_terminal_sink_build_succeeds() {
    workflow::GraphBuilder b("t9");
    AgentConfig cfg;
    cfg.system_prompt = "sys";
    AgentWorkflowDeps deps;
    deps.llm = std::make_shared<LLMClient>();
    deps.toolbus = std::make_shared<ToolBus>();
    auto st = std::make_shared<internal::AgentThreadState>();
    st->initial_user_prompt = "hello";
    bool called_local = false;
    CliAgentTerminalSinkOptions sink;
    sink.on_final_json = [&called_local](const json& j) {
        (void)j;
        called_local = true;
    };
    build_cli_agent_graph_with_terminal_sink(b, cfg, deps, st, sink);
    assert(!called_local);

    GraphExecutor ex;
    workflow::GraphBuilder b2("t10");
    ex.build_agent_workflow(cfg, b2, deps, st, sink);
}

void test_terminal_sink_user_query_overload() {
    workflow::GraphBuilder b("t11");
    AgentConfig cfg;
    cfg.system_prompt = "sys";
    AgentWorkflowDeps deps;
    deps.llm = std::make_shared<LLMClient>();
    deps.toolbus = std::make_shared<ToolBus>();
    CliAgentTerminalSinkOptions sink;
    sink.sink_node_name = "MySink";
    sink.on_final_json = [](const json&) {};
    build_cli_agent_graph_with_terminal_sink(b, cfg, deps, "q", sink);
}

} // namespace

int main() {
    test_null_llm_throws();
    test_null_toolbus_throws();
    test_null_state_throws();
    test_build_succeeds_minimal();
    test_user_query_overload();
    test_react_template_build_throws();
    test_terminal_sink_empty_callback_throws();
    test_terminal_sink_build_succeeds();
    test_terminal_sink_user_query_overload();
    std::cout << "test_build_cli_agent_graph: all passed\n";
    return 0;
}
