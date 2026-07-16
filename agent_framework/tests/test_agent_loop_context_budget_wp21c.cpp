/**
 * @file test_agent_loop_context_budget_wp21c.cpp
 * @brief WP2.1c 集成 I-1：AgentLoop + 超大工具返回，单工具帽后仍可完成
 */

#include <agent/graph_executor/graph_executor.hpp>
#include <agent/llm_client/llm_client.hpp>
#include <agent/prompt_renderer/prompt_renderer.hpp>
#include <agent/toolbus/toolbus.hpp>
#include <agent/core/types.hpp>
#include <agent/context_budget/context_budget.hpp>
#include <agent/internal/agent_thread_state.hpp>

#include <cassert>
#include <cstdlib>
#include <future>
#include <memory>
#include <string>

#include <taskflow/taskflow.hpp>
#include <workflow/nodeflow.hpp>

namespace {

using json = nlohmann::json;
using namespace agent_framework;

class FakeTwoStepAdapter final : public ModelAdapter {
public:
    std::future<LLMOutput> invoke(const LLMInput& /*input*/,
                                  std::function<void(std::string_view)> /*stream_callback*/) override {
        return make_out();
    }

    std::future<LLMOutput> invoke_with_rendered(
        const RenderedPrompt& /*rendered*/,
        std::function<void(std::string_view)> /*stream_callback*/) override {
        return make_out();
    }

    std::vector<ToolMeta> get_available_tools() const override {
        return {};
    }

    void configure(const ModelConfig& /*config*/) override {}

    std::string get_model_name() const override {
        return "fake-huge-tool-wp21c";
    }

    bool supports_multimodal() const override {
        return false;
    }

private:
    std::future<LLMOutput> make_out() {
        const int p = phase_++;
        return std::async(std::launch::deferred, [p]() {
            LLMOutput o;
            if (p == 0) {
                CallSpec c;
                c.name = "huge_tool";
                c.arguments = json::object();
                o.tool_calls.push_back(std::move(c));
                o.is_final = false;
            } else {
                o.is_final = true;
                o.final_answer = "done-after-tool";
            }
            return o;
        });
    }

    int phase_ = 0;
};

void test_i1_agent_loop_huge_tool_result() {
#if defined(_WIN32)
    (void)_putenv_s("AGENT_BUDGET_MAX_TOOL_RESULT_JSON_BYTES", "8192");
#else
    (void)::setenv("AGENT_BUDGET_MAX_TOOL_RESULT_JSON_BYTES", "8192", 1);
#endif

    auto llm = std::make_shared<LLMClient>();
    llm->set_prompt_renderer(std::make_shared<PromptRenderer>());
    llm->register_adapter("fake", std::make_shared<FakeTwoStepAdapter>());
    llm->set_default_adapter("fake");

    auto bus = std::make_shared<ToolBus>();
    ToolMeta meta;
    meta.name = "huge_tool";
    meta.schema = json::parse(R"({"type":"object","properties":{}})");
    meta.description = "returns large json";
    bus->register_local_tool(
        "huge_tool",
        [](const json& /*j*/) {
            return json{{"blob", std::string(200000, 'x')}, {"ok", true}};
        },
        meta);

    AgentWorkflowDeps deps;
    deps.llm = llm;
    deps.toolbus = bus;

    AgentConfig cfg;
    cfg.name = "wp21c_ctx_budget";
    cfg.system_prompt = "test";
    cfg.max_iterations = 5;
    cfg.max_tool_calls_per_iteration = 4;

    tf::Executor ex;
    workflow::GraphBuilder b("wp21c_ctx_budget");

    auto st = std::make_shared<internal::AgentThreadState>();
    st->initial_user_prompt = "run tool";

    json final;
    std::shared_ptr<internal::AgentThreadState> final_state;
    CliAgentTerminalSinkOptions sink;
    sink.sink_node_name = "Sink";
    sink.on_final_json = [&final](const json& j) { final = j; };
    sink.on_final_state = [&final_state](const std::shared_ptr<internal::AgentThreadState>& s) {
        final_state = s;
    };

    build_cli_agent_graph_with_terminal_sink(b, cfg, deps, st, sink);

    auto fut = b.run_async(ex);
    fut.wait();

    assert(final.is_object());
    assert(final.contains("final_answer"));
    assert(final_state);
    bool found_tool = false;
    for (const auto& m : final_state->history) {
        if (m.role == "tool" && m.tool_name && *m.tool_name == "huge_tool" && m.tool_result) {
            found_tool = true;
            const std::size_t bytes = json_utf8_dump_bytes(*m.tool_result);
            assert(bytes <= 12000);
            assert(m.tool_result->contains("_af_truncation") || bytes < 50000);
        }
    }
    assert(found_tool);

#if defined(_WIN32)
    (void)_putenv_s("AGENT_BUDGET_MAX_TOOL_RESULT_JSON_BYTES", "");
#else
    (void)::unsetenv("AGENT_BUDGET_MAX_TOOL_RESULT_JSON_BYTES");
#endif
}

} // namespace

int main() {
    test_i1_agent_loop_huge_tool_result();
    return 0;
}
