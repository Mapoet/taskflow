/**
 * @file test_verifier_runner.cpp
 * @brief WP2.8 Verifier runner smoke (mock LLM, no network)
 */
#include <agent/internal/agent_thread_state.hpp>
#include <agent/llm_client.hpp>
#include <agent/prompt_renderer.hpp>
#include <agent/types.hpp>
#include <agent/verifier_runner.hpp>

#include <cassert>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>

namespace {

using namespace agent_framework;

class FixedJsonVerifierAdapter final : public ModelAdapter {
public:
    explicit FixedJsonVerifierAdapter(std::string body) : body_(std::move(body)) {}

    std::future<LLMOutput> invoke(
        const LLMInput& /*input*/,
        std::function<void(std::string_view)> /*stream_callback*/ = nullptr) override {
        return std::async(std::launch::deferred, [this]() {
            LLMOutput o;
            o.is_final = true;
            o.final_answer = body_;
            return o;
        });
    }

    std::future<LLMOutput> invoke_with_rendered(
        const RenderedPrompt& /*rendered*/,
        std::function<void(std::string_view)> /*stream_callback*/ = nullptr) override {
        return std::async(std::launch::deferred, [this]() {
            LLMOutput o;
            o.is_final = true;
            o.final_answer = body_;
            o.tool_calls.clear();
            return o;
        });
    }

    std::vector<ToolMeta> get_available_tools() const override {
        return {};
    }

    void configure(const ModelConfig& /*config*/) override {}

    std::string get_model_name() const override {
        return "fake-verifier";
    }

    bool supports_multimodal() const override {
        return false;
    }

private:
    std::string body_;
};

void test_build_user_json_smoke() {
    internal::AgentThreadState st;
    st.initial_user_prompt = "hello";
    Message um;
    um.role = "user";
    um.content = "hello";
    um.timestamp = std::time(nullptr);
    st.history.push_back(std::move(um));
    std::string j = build_verifier_user_json(st, "draft text", 4096);
    assert(j.find("hello") != std::string::npos);
    assert(j.find("draft text") != std::string::npos);
}

void test_run_verifier_sync_pass() {
    auto ad = std::make_shared<FixedJsonVerifierAdapter>(R"({"ok":true,"issues":[],"suggested_action":"pass"})");
    auto llm = std::make_shared<LLMClient>();
    llm->set_prompt_renderer(std::make_shared<PromptRenderer>());
    llm->register_adapter("fake", ad);
    llm->set_default_adapter("fake");
    std::string payload = R"({"user_query":"q","draft_final_answer":"x","history_digest":[]})";
    auto out = run_verifier_sync(*llm, payload, 0, 1, 5000);
    assert(out.parsed.ok);
    assert(out.parsed.suggested_action == "pass");
}

void test_run_verifier_sync_timeout_zero_still_runs() {
    auto ad = std::make_shared<FixedJsonVerifierAdapter>(R"({"ok":false,"issues":[],"suggested_action":"pass_through"})");
    auto llm = std::make_shared<LLMClient>();
    llm->set_prompt_renderer(std::make_shared<PromptRenderer>());
    llm->register_adapter("fake", ad);
    llm->set_default_adapter("fake");
    auto out = run_verifier_sync(*llm, "{}", 0, 1, 0);
    assert(!out.parsed.ok);
}

} // namespace

int main() {
    test_build_user_json_smoke();
    test_run_verifier_sync_pass();
    test_run_verifier_sync_timeout_zero_still_runs();
    std::cout << "test_verifier_runner: all passed\n";
    return EXIT_SUCCESS;
}
