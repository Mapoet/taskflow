/**
 * @file test_agent_loop_wp5.cpp
 * @brief WP1.5 Agent loop integration test (mock LLM, real ToolBus)
 */

#include <agent/llm_client.hpp>
#include <agent/prompt_renderer.hpp>
#include <agent/toolbus.hpp>
#include <agent/types.hpp>
#include <agent/internal/agent_thread_state.hpp>
#include <agent/internal/loop_io_keys.hpp>
#include <node/agent_loop_node.hpp>

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include <taskflow/taskflow.hpp>
#include <workflow/nodeflow.hpp>

namespace {

using json = nlohmann::json;
using namespace agent_framework;

std::string env_or(const char* key, const char* default_val) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : std::string(default_val);
}

bool env_truthy(const char* key) {
    const char* v = std::getenv(key);
    if (!v || !*v) {
        return false;
    }
    return v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T';
}

bool is_debug_enabled() {
    const char* v = std::getenv("AGENT_TEST_AGENT_LOOP_DEBUG");
    return v && std::string(v) != "0";
}

void dump_llm_output(const LLMOutput& o) {
    std::cout << "  llm.is_final=" << (o.is_final ? "true" : "false")
              << " final_answer_len=" << o.final_answer.size()
              << " tool_calls=" << o.tool_calls.size() << "\n";
    for (std::size_t i = 0; i < o.tool_calls.size(); ++i) {
        const auto& c = o.tool_calls[i];
        std::cout << "    [" << i << "] tool=" << c.name
                  << " id=" << (c.tool_call_id ? *c.tool_call_id : std::string("(none)"))
                  << " args=" << c.arguments.dump() << "\n";
    }
    if (!o.final_answer.empty()) {
        std::cout << "  llm.final_answer=\"" << o.final_answer << "\"\n";
    }
}

void test_agent_loop_live() {
    const bool dbg = is_debug_enabled();
    if (dbg) {
        std::cout << "== WP1.5 agent loop debug ==\n";
        std::cout << "env AGENT_TEST_AGENT_LOOP_DEBUG=1 enabled\n";
    }

    // Align with WP1.1 live test conventions:
    // - Use DEEPSEEK_API_KEY as alias for OPENAI_API_KEY when present.
    // - Default DeepSeek OpenAI-compatible base URL if not set.
    if (std::getenv("OPENAI_API_KEY") == nullptr) {
        const char* dk = std::getenv("DEEPSEEK_API_KEY");
        if (dk && *dk) {
            (void)::setenv("OPENAI_API_KEY", dk, 0);
        }
    }
    if (std::getenv("AGENT_OPENAI_BASE_URL") == nullptr) {
        (void)::setenv("AGENT_OPENAI_BASE_URL", "https://api.deepseek.com/v1", 0);
    }
    if (std::getenv("AGENT_LLM_PROVIDER") == nullptr) {
        (void)::setenv("AGENT_LLM_PROVIDER", "openai", 0);
    }
    if (std::getenv("AGENT_LLM_MODEL") == nullptr) {
        const std::string m = env_or("DEEPSEEK_MODEL", "deepseek-chat");
        (void)::setenv("AGENT_LLM_MODEL", m.c_str(), 0);
    }

    // ToolBus with local add tool
    auto bus = std::make_shared<ToolBus>();
    ToolMeta add_meta;
    add_meta.name = "add";
    add_meta.description = "add";
    add_meta.schema = json{
        {"type", "object"},
        {"properties", {{"a", {{"type", "integer"}}}, {"b", {{"type", "integer"}}}}},
        {"required", {"a", "b"}}};

    bus->register_local_tool(
        "add",
        [](const json& args) -> json {
            return json{{"result", args.at("a").get<int>() + args.at("b").get<int>()}};
        },
        add_meta);

    // Best-effort import Cursor MCP tools
    const std::string cursor_mcp =
        env_or("AGENT_TEST_CURSOR_MCP_JSON", "/home/mapoet/.cursor/mcp.json");
    try {
        (void)::setenv("AGENT_MCP_REQUEST_TIMEOUT_MS", "5000", 1);
        bus->register_mcp_from_cursor_config(cursor_mcp, true);
    } catch (...) {
        // ignore
    }

    // LLMClient live
    auto llm = std::make_shared<LLMClient>(LLMClient::from_env());

    // PromptRenderer
    auto renderer = std::make_shared<PromptRenderer>();
    llm->set_prompt_renderer(renderer);

    // Agent config
    AgentConfig cfg;
    cfg.name = "t";
    cfg.system_prompt =
        "You are a tool-using agent.\n"
        "You MUST call tool `add` exactly once to compute 1+2.\n"
        "Then answer with the number only and do NOT call any more tools.\n";
    cfg.model_config.model_name = env_or("AGENT_LLM_MODEL", "deepseek-chat");
    cfg.max_iterations = 5;
    cfg.max_tool_calls_per_iteration = 5;

    tf::Executor ex;
    workflow::GraphBuilder b("wp5_test");

    auto init_state = std::make_shared<internal::AgentThreadState>();
    init_state->initial_user_prompt = "Please compute 1+2 and answer with the number only.";
    if (dbg) {
        std::cout << "init_state.iteration=" << init_state->iteration
                  << " history_size=" << init_state->history.size()
                  << " initial_user_prompt=\"" << init_state->initial_user_prompt << "\"\n";
        std::cout << "cfg.model=\"" << cfg.model_config.model_name << "\""
                  << " max_iterations=" << cfg.max_iterations
                  << " max_tool_calls_per_iteration=" << cfg.max_tool_calls_per_iteration << "\n";
    }

    auto [sys_src, _st] = b.create_any_source(
        "SystemPrompt",
        std::unordered_map<std::string, std::any>{{std::string(internal::kSystemPrompt),
                                                   std::any{cfg.system_prompt}}});
    (void)sys_src;
    auto [user_src, _ut] = b.create_any_source(
        "UserInput",
        std::unordered_map<std::string, std::any>{{std::string(internal::kUserQuery),
                                                   std::any{init_state->initial_user_prompt}}});
    (void)user_src;
    auto [state_src, _at] = b.create_any_source(
        "AgentState",
        std::unordered_map<std::string, std::any>{{std::string(internal::kAgentState),
                                                   std::any{init_state}}});
    (void)state_src;

    // Create loop node
    auto [loop_node, loop_task] = node::AgentLoopNode::create(
        b,
        "AgentLoop",
        cfg,
        llm,
        bus,
        nullptr,
        nullptr,
        {{"SystemPrompt", std::string(internal::kSystemPrompt)},
         {"UserInput", std::string(internal::kUserQuery)},
         {"AgentState", std::string(internal::kAgentState)}},
        {std::string(internal::kFinalAnswer),
         std::string(internal::kNextAgentState),
         std::string(internal::kLlmOutput)});
    (void)loop_task;
    (void)loop_node;

    std::string final_answer;
    auto [sink, sink_task] = b.create_any_sink(
        "Sink",
        {{"AgentLoop", std::string(internal::kFinalAnswer)},
         {"AgentLoop", std::string(internal::kNextAgentState)}},
        [&final_answer, dbg](const std::unordered_map<std::string, std::any>& outs) {
            final_answer = std::any_cast<std::string>(outs.at(std::string(internal::kFinalAnswer)));
            auto st = std::any_cast<std::shared_ptr<internal::AgentThreadState>>(
                outs.at(std::string(internal::kNextAgentState)));
            assert(st);
            assert(st->iteration >= 3);
            if (dbg) {
                std::cout << "== loop exited ==\n";
                std::cout << "final_answer=\"" << final_answer << "\"\n";
                std::cout << "final_state.iteration=" << st->iteration
                          << " history_size=" << st->history.size() << "\n";
                // show tail of history (up to last 6)
                const std::size_t n = st->history.size();
                const std::size_t start = (n > 6) ? (n - 6) : 0;
                for (std::size_t i = start; i < n; ++i) {
                    const auto& m = st->history[i];
                    std::cout << "  hist[" << i << "] role=" << m.role;
                    if (m.tool_name) {
                        std::cout << " tool_name=" << *m.tool_name;
                    }
                    if (!m.content.empty()) {
                        std::cout << " content_len=" << m.content.size();
                    }
                    if (m.tool_result) {
                        std::cout << " tool_result=" << m.tool_result->dump();
                    }
                    std::cout << "\n";
                }
            }
        });
    (void)sink;
    (void)sink_task;

    auto f = b.run_async(ex);
    f.wait();
    if (!dbg) {
        std::cout << "final_answer:\n" << final_answer << "\n";
    }
    if (final_answer.find("3") == std::string::npos) {
        throw std::runtime_error("expected final_answer to contain 3, got: " + final_answer);
    }
    if (dbg) {
        std::cout << "PASS\n";
    }
}

} // namespace

int main() {
    if (!env_truthy("AGENT_TEST_LIVE")) {
        std::cout << "test_agent_loop_wp5: skipped (set AGENT_TEST_LIVE=1 to run live)\n";
        return 0;
    }
    test_agent_loop_live();
    return 0;
}

