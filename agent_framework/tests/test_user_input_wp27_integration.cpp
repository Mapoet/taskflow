/**
 * @file test_user_input_wp27_integration.cpp
 * @brief WP2.7 I-1：AgentLoop 首轮将 pending 注入拼入 LLMInput.context
 */
#include <agent/graph_executor.hpp>
#include <agent/internal/agent_thread_state.hpp>
#include <agent/llm_client.hpp>
#include <agent/prompt_renderer.hpp>
#include <agent/toolbus.hpp>
#include <agent/types.hpp>

#include <cassert>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <string>

#include <taskflow/taskflow.hpp>
#include <workflow/nodeflow.hpp>

namespace {

using namespace agent_framework;

class CaptureLlmAdapter final : public ModelAdapter {
public:
    LLMInput last_in;
    RenderedPrompt last_rendered;
    std::future<LLMOutput> invoke(
        const LLMInput& input,
        std::function<void(std::string_view)> /*stream_callback*/ = nullptr) override {
        last_in = input;
        return std::async(std::launch::deferred, [] {
            LLMOutput o;
            o.is_final = true;
            o.final_answer = "ok";
            return o;
        });
    }

    std::future<LLMOutput> invoke_with_rendered(
        const RenderedPrompt& rendered,
        std::function<void(std::string_view)> /*stream_callback*/ = nullptr) override {
        last_rendered = rendered;
        return std::async(std::launch::deferred, [] {
            LLMOutput o;
            o.is_final = true;
            o.final_answer = "ok";
            return o;
        });
    }

    std::vector<ToolMeta> get_available_tools() const override {
        return {};
    }
    void configure(const ModelConfig& /*config*/) override {}
    std::string get_model_name() const override {
        return "fake-wp27";
    }
    bool supports_multimodal() const override {
        return false;
    }
};

void i1_pending_injection_reaches_llm_context() {
    auto cap = std::make_shared<CaptureLlmAdapter>();
    auto llm = std::make_shared<LLMClient>();
    llm->set_prompt_renderer(std::make_shared<PromptRenderer>());
    llm->register_adapter("cap", cap);
    llm->set_default_adapter("cap");

    auto bus = std::make_shared<ToolBus>();

    AgentWorkflowDeps deps;
    deps.llm = llm;
    deps.toolbus = bus;

    AgentConfig cfg;
    cfg.name = "wp27_i1";
    cfg.system_prompt = "sys";
    cfg.max_iterations = 1;

    auto st = std::make_shared<internal::AgentThreadState>();
    st->initial_user_prompt = "user line";
    InjectedContextBlock b;
    b.source_kind = "file";
    b.source_ref = "note.txt";
    b.text_utf8 = "SECRET_INJECT";
    b.byte_length = b.text_utf8.size();
    st->pending_injected_context.push_back(std::move(b));

    json final;
    CliAgentTerminalSinkOptions sink;
    sink.on_final_json = [&final](const json& j) { final = j; };

    tf::Executor ex;
    workflow::GraphBuilder bld("wp27_i1");
    build_cli_agent_graph_with_terminal_sink(bld, cfg, deps, st, sink);
    auto fut = bld.run_async(ex);
    fut.wait();

    assert(final.is_object());
    // LLMClient::invoke renders first, then calls invoke_with_rendered (not invoke).
    assert(cap->last_rendered.rendered_text.find("--- injection:file:") != std::string::npos);
    assert(cap->last_rendered.rendered_text.find("SECRET_INJECT") != std::string::npos);
    assert(st->pending_injected_context.empty());
}

} // namespace

int main() {
    (void)::setenv("AGENT_TOOL_ALLOWLIST", "", 1);
    i1_pending_injection_reaches_llm_context();
    std::cout << "test_user_input_wp27_integration: ok\n";
    return 0;
}
