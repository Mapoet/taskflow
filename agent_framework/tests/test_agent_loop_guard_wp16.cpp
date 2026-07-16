/**
 * @file test_agent_loop_guard_wp16.cpp
 * @brief WP1.6 guard smoke: repeated tool call within one iteration triggers graceful final.
 */

#include <agent/graph_executor/graph_executor.hpp>
#include <agent/llm_client/llm_client.hpp>
#include <agent/prompt_renderer/prompt_renderer.hpp>
#include <agent/toolbus/toolbus.hpp>
#include <agent/core/types.hpp>
#include <agent/internal/agent_thread_state.hpp>

#include <cassert>
#include <future>
#include <memory>
#include <string>

#include <taskflow/taskflow.hpp>
#include <workflow/nodeflow.hpp>

namespace {

using json = nlohmann::json;
using namespace agent_framework;

class FakeRepeatToolAdapter final : public ModelAdapter {
public:
    std::future<LLMOutput> invoke(const LLMInput& /*input*/,
                                 std::function<void(std::string_view)> /*stream_callback*/) override {
        return std::async(std::launch::async, []() {
            LLMOutput out;
            out.is_final = false;
            out.final_answer = "";
            out.reasoning = "call tool twice (same args) to trigger guard";
            CallSpec c;
            c.name = "guard_test_tool";
            c.arguments = json{{"x", 1}};
            out.tool_calls = {c, c};  // exact repeat within one iteration
            return out;
        });
    }

    std::future<LLMOutput> invoke_with_rendered(
        const RenderedPrompt& /*rendered*/,
        std::function<void(std::string_view)> /*stream_callback*/) override {
        // Delegate to invoke for this fake adapter.
        return invoke(LLMInput{}, nullptr);
    }

    std::vector<ToolMeta> get_available_tools() const override {
        return {};
    }

    void configure(const ModelConfig& /*config*/) override {}

    std::string get_model_name() const override {
        return "fake-repeat-tool";
    }

    bool supports_multimodal() const override {
        return false;
    }
};

void test_repeat_tool_in_iteration_triggers_guard() {
    // Ensure the repeat guard is enabled.
#if defined(_WIN32)
    (void)_putenv_s("AGENT_LOOP_GUARD_REPEAT_TOOL_IN_ITERATION", "1");
#else
    (void)::setenv("AGENT_LOOP_GUARD_REPEAT_TOOL_IN_ITERATION", "1", 1);
#endif

    auto llm = std::make_shared<LLMClient>();
    llm->set_prompt_renderer(std::make_shared<PromptRenderer>());
    llm->register_adapter("fake", std::make_shared<FakeRepeatToolAdapter>());
    llm->set_default_adapter("fake");

    auto bus = std::make_shared<ToolBus>();
    ToolMeta meta;
    meta.name = "guard_test_tool";
    meta.schema = json::parse(
        R"({"type":"object","properties":{"x":{"type":"integer"}},"required":["x"]})");
    meta.description = "tool used by guard test";
    bus->register_local_tool(
        "guard_test_tool",
        [](const json& j) { return json{{"ok", true}, {"x", j.at("x")}}; },
        meta);

    AgentWorkflowDeps deps;
    deps.llm = llm;
    deps.toolbus = bus;

    AgentConfig cfg;
    cfg.name = "guard_wp16";
    cfg.system_prompt = "test";
    cfg.max_iterations = 10;
    cfg.max_tool_calls_per_iteration = 8;

    tf::Executor ex;
    workflow::GraphBuilder b("guard_wp16");

    auto st = std::make_shared<internal::AgentThreadState>();
    st->initial_user_prompt = "hi";

    json final;
    CliAgentTerminalSinkOptions sink;
    sink.sink_node_name = "Sink";
    sink.on_final_json = [&final](const json& j) { final = j; };

    build_cli_agent_graph_with_terminal_sink(b, cfg, deps, st, sink);

    auto fut = b.run_async(ex);
    fut.wait();

    assert(final.is_object());
    assert(final.contains("final_answer"));
    const std::string fa = final.at("final_answer").get<std::string>();
    assert(fa.rfind("[guard]", 0) == 0);
    assert(final.contains("guard_triggered"));
    assert(final.at("guard_triggered").get<bool>() == true);
    assert(final.contains("guard_reason"));
    assert(final.at("guard_reason").get<std::string>() == "repeat_tool_call_in_iteration");
}

} // namespace

int main() {
    test_repeat_tool_in_iteration_triggers_guard();
    return 0;
}

