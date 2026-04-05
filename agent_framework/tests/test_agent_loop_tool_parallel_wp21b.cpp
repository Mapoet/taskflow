/**
 * @file test_agent_loop_tool_parallel_wp21b.cpp
 * @brief I-2: Agent loop with two ReadOnly tools + parallel reads enabled (order + smoke).
 */

#include <agent/graph_executor.hpp>
#include <agent/llm_client.hpp>
#include <agent/prompt_renderer.hpp>
#include <agent/toolbus.hpp>
#include <agent/types.hpp>
#include <agent/internal/agent_thread_state.hpp>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <taskflow/taskflow.hpp>
#include <workflow/nodeflow.hpp>

namespace {

using json = nlohmann::json;
using namespace agent_framework;

ToolMeta meta_named(const std::string& name, ToolSideEffect se) {
    ToolMeta m;
    m.name = name;
    m.description = name;
    m.schema = json{{"type", "object"}, {"properties", json::object()}};
    m.side_effect = se;
    return m;
}

class FakeTwoReadToolsAdapter final : public ModelAdapter {
public:
    std::future<LLMOutput> invoke(const LLMInput& /*input*/,
                                    std::function<void(std::string_view)> /*stream_callback*/) override {
        return std::async(std::launch::async, [this]() {
            LLMOutput out;
            if (phase_ == 0) {
                ++phase_;
                out.is_final = false;
                CallSpec a;
                a.name = "parallel_ro_a";
                a.arguments = json::object();
                CallSpec b;
                b.name = "parallel_ro_b";
                b.arguments = json::object();
                out.tool_calls = {std::move(a), std::move(b)};
                return out;
            }
            out.is_final = true;
            out.final_answer = "ok";
            out.tool_calls.clear();
            return out;
        });
    }

    std::future<LLMOutput> invoke_with_rendered(
        const RenderedPrompt& /*rendered*/,
        std::function<void(std::string_view)> /*stream_callback*/) override {
        return invoke(LLMInput{}, nullptr);
    }

    std::vector<ToolMeta> get_available_tools() const override {
        return {};
    }

    void configure(const ModelConfig& /*config*/) override {}

    std::string get_model_name() const override {
        return "fake-two-read-tools";
    }

    bool supports_multimodal() const override {
        return false;
    }

private:
    int phase_ = 0;
};

bool history_has_tool_order(const std::vector<Message>& history,
                            const std::vector<std::string>& expected_names) {
    std::vector<std::string> got;
    for (const auto& m : history) {
        if (m.role == "tool" && m.tool_name.has_value()) {
            got.push_back(*m.tool_name);
        }
    }
    if (got.size() < expected_names.size()) {
        return false;
    }
    // Allow extra tool msgs from other tests? No — fresh state.
    for (std::size_t i = 0; i < expected_names.size(); ++i) {
        if (got[i] != expected_names[i]) {
            return false;
        }
    }
    return true;
}

void test_parallel_two_readonly_tools_order() {
    auto llm = std::make_shared<LLMClient>();
    llm->set_prompt_renderer(std::make_shared<PromptRenderer>());
    llm->register_adapter("fake", std::make_shared<FakeTwoReadToolsAdapter>());
    llm->set_default_adapter("fake");

    auto bus = std::make_shared<ToolBus>();
    auto reg_ro = [&](const char* name) {
        const std::string nm(name);
        ToolMeta meta = meta_named(nm, ToolSideEffect::ReadOnly);
        bus->register_local_tool(
            nm,
            [nm](const json& /*j*/) {
                std::this_thread::sleep_for(std::chrono::milliseconds(45));
                return json{{"tool", nm}};
            },
            meta);
    };
    reg_ro("parallel_ro_a");
    reg_ro("parallel_ro_b");

    AgentWorkflowDeps deps;
    deps.llm = llm;
    deps.toolbus = bus;

    AgentConfig cfg;
    cfg.name = "wp21b_parallel";
    cfg.system_prompt = "test";
    cfg.max_iterations = 5;
    cfg.max_tool_calls_per_iteration = 8;
    cfg.enable_parallel_read_tools = true;
    cfg.max_parallel_read_tools = 4;

    tf::Executor ex;
    workflow::GraphBuilder b("wp21b_parallel");

    auto st = std::make_shared<internal::AgentThreadState>();
    st->initial_user_prompt = "hi";

    std::shared_ptr<internal::AgentThreadState> final_state;
    json final_json;
    CliAgentTerminalSinkOptions sink;
    sink.sink_node_name = "Sink";
    sink.on_final_json = [&final_json](const json& j) { final_json = j; };
    sink.on_final_state = [&final_state](const std::shared_ptr<internal::AgentThreadState>& s) {
        final_state = s;
    };

    const auto t0 = std::chrono::steady_clock::now();
    build_cli_agent_graph_with_terminal_sink(b, cfg, deps, st, sink);
    b.run_async(ex).wait();
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();

    assert(final_state);
    assert(history_has_tool_order(final_state->history, {"parallel_ro_a", "parallel_ro_b"}));
    // Parallel overlap: two ~45ms reads overlapped should beat ~90ms serial (generous margin for CI).
    if (ms >= 115) {
        throw std::runtime_error("expected parallel read overlap (wall ms=" + std::to_string(ms) + ")");
    }
    (void)final_json;
}

} // namespace

int main() {
#if defined(_WIN32)
    (void)_putenv_s("AGENT_TOOL_ALLOWLIST", "");
    (void)_putenv_s("AGENT_TOOL_PARALLEL_READS", "1");
    (void)_putenv_s("AGENT_TOOL_MAX_PARALLEL", "4");
#else
    (void)::setenv("AGENT_TOOL_ALLOWLIST", "", 1);
    (void)::setenv("AGENT_TOOL_PARALLEL_READS", "1", 1);
    (void)::setenv("AGENT_TOOL_MAX_PARALLEL", "4", 1);
#endif
    test_parallel_two_readonly_tools_order();
    return 0;
}
