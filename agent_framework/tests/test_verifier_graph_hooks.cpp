/**
 * @file test_verifier_graph_hooks.cpp
 * @brief WP2.8 I-1：run_react_cli_sync + Verifier 钩子事件（mock MAIN / mock Verifier LLM）
 */

#include <agent/execution_context.hpp>
#include <agent/graph_executor.hpp>
#include <agent/internal/agent_thread_state.hpp>
#include <agent/llm_client.hpp>
#include <agent/prompt_renderer.hpp>
#include <agent/toolbus.hpp>
#include <agent/types.hpp>

#include <cassert>
#include <cstdlib>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <iostream>
#include <taskflow/taskflow.hpp>

namespace {

using namespace agent_framework;

class OneShotFinalAdapter final : public ModelAdapter {
public:
    std::future<LLMOutput> invoke(
        const LLMInput& /*input*/,
        std::function<void(std::string_view)> /*stream_callback*/ = nullptr) override {
        return std::async(std::launch::async, []() {
            LLMOutput out;
            out.is_final = true;
            out.final_answer = "main-draft";
            return out;
        });
    }

    std::future<LLMOutput> invoke_with_rendered(
        const RenderedPrompt& /*rendered*/,
        std::function<void(std::string_view)> /*stream_callback*/ = nullptr) override {
        return std::async(std::launch::async, []() {
            LLMOutput out;
            out.is_final = true;
            out.final_answer = "main-draft";
            return out;
        });
    }

    std::vector<ToolMeta> get_available_tools() const override {
        return {};
    }
    void configure(const ModelConfig& /*config*/) override {}
    std::string get_model_name() const override {
        return "fake-oneshot-mock";
    }
    bool supports_multimodal() const override {
        return false;
    }
};

class VerifierPassJsonAdapter final : public ModelAdapter {
public:
    std::future<LLMOutput> invoke(
        const LLMInput& /*input*/,
        std::function<void(std::string_view)> /*stream_callback*/ = nullptr) override {
        return make_pass();
    }

    std::future<LLMOutput> invoke_with_rendered(
        const RenderedPrompt& /*rendered*/,
        std::function<void(std::string_view)> /*stream_callback*/ = nullptr) override {
        return make_pass();
    }

    std::vector<ToolMeta> get_available_tools() const override {
        return {};
    }
    void configure(const ModelConfig& /*config*/) override {}
    std::string get_model_name() const override {
        return "fake-verifier-mock";
    }
    bool supports_multimodal() const override {
        return false;
    }

private:
    static std::future<LLMOutput> make_pass() {
        return std::async(std::launch::async, []() {
            LLMOutput out;
            out.is_final = true;
            out.final_answer =
                R"json({"ok":true,"issues":[],"suggested_action":"pass"})json";
            return out;
        });
    }
};

void test_verifier_events_and_outputs() {
    (void)::setenv("AGENT_VERIFIER", "on", 1);
    (void)::setenv("AGENT_VERIFIER_MAX_RETRIES", "1", 1);
    (void)::setenv("AGENT_TOOL_ALLOWLIST", "", 1);

    auto main_adp = std::make_shared<OneShotFinalAdapter>();
    auto main_llm = std::make_shared<LLMClient>();
    main_llm->set_prompt_renderer(std::make_shared<PromptRenderer>());
    main_llm->register_adapter("fake", main_adp);
    main_llm->set_default_adapter("fake");

    auto ver_adp = std::make_shared<VerifierPassJsonAdapter>();
    auto ver_llm = std::make_shared<LLMClient>();
    ver_llm->set_prompt_renderer(std::make_shared<PromptRenderer>());
    ver_llm->register_adapter("fake", ver_adp);
    ver_llm->set_default_adapter("fake");

    auto bus = std::make_shared<ToolBus>();
    AgentWorkflowDeps deps;
    deps.llm = main_llm;
    deps.toolbus = bus;

    AgentConfig cfg;
    cfg.system_prompt = "sys";
    cfg.max_iterations = 4;

    auto session = std::make_shared<internal::AgentThreadState>();
    session->initial_user_prompt = "hello";
    session->execution_context = ExecutionContext{};
    session->execution_context->task_id = "tid-hook";
    session->execution_context->session_id = "sid-hook";

    std::mutex cap_mu;
    std::vector<std::pair<std::string, json>> captured;

    tf::Executor executor;
    GraphExecutor gx;
    ReactCliRunRequest req;
    req.config = cfg;
    req.deps = deps;
    req.session = session;
    req.options.sink.on_final_json = [](const json&) {};
    req.options.verifier_llm_override = ver_llm;
    req.options.on_verifier_event = [&](std::string_view name, const json& pl) {
        std::lock_guard<std::mutex> lk(cap_mu);
        captured.emplace_back(std::string(name), pl);
    };

    WorkflowResult wr = gx.run_react_cli_sync(executor, req);
    assert(wr.success);
    assert(wr.outputs.contains("verifier_ok"));
    assert(wr.outputs.at("verifier_ok").get<bool>() == true);
    assert(wr.outputs.at("final_answer").get<std::string>() == "main-draft");

    std::lock_guard<std::mutex> lk(cap_mu);
    assert(captured.size() == 2U);
    assert(captured[0].first == "verifier_started");
    assert(captured[1].first == "verifier_completed");
    assert(captured[0].second.at("task_id").get<std::string>() == "tid-hook");
}

} // namespace

int main() {
    test_verifier_events_and_outputs();
    (void)::setenv("AGENT_VERIFIER", "off", 1);
    std::cout << "test_verifier_graph_hooks: all passed\n";
    return 0;
}
