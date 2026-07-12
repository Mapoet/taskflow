#ifndef AGENT_FRAMEWORK_TEST_EXECUTION_PROFILE_HPP
#define AGENT_FRAMEWORK_TEST_EXECUTION_PROFILE_HPP

#include <agent/agent_server.hpp>
#include <agent/llm_client.hpp>
#include <agent/prompt_renderer.hpp>
#include <agent/toolbus.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

namespace agent_framework::test {

using TestReply = std::function<std::string(const RenderedPrompt&)>;

class ServerTestAdapter final : public ModelAdapter {
public:
    explicit ServerTestAdapter(TestReply reply) : reply_(std::move(reply)) {}

    std::future<LLMOutput> invoke(const LLMInput& input,
                                 std::function<void(std::string_view)> = nullptr) override {
        RenderedPrompt rendered;
        for (const auto& message : input.history) {
            rendered.messages.push_back({{"role", message.role}, {"content", message.content}});
        }
        return invoke_with_rendered(rendered);
    }

    std::future<LLMOutput> invoke_with_rendered(
        const RenderedPrompt& rendered,
        std::function<void(std::string_view)> = nullptr) override {
        return std::async(std::launch::async, [reply = reply_, rendered] {
            LLMOutput output;
            output.is_final = true;
            output.final_answer = reply ? reply(rendered) : "ok";
            return output;
        });
    }

    std::vector<ToolMeta> get_available_tools() const override { return {}; }
    void configure(const ModelConfig&) override {}
    std::string get_model_name() const override { return "server-test-adapter"; }
    bool supports_multimodal() const override { return false; }

private:
    TestReply reply_;
};

inline void configure_execution_profile(AgentServer& server, TestReply reply = {}) {
    auto llm = std::make_shared<LLMClient>();
    llm->set_prompt_renderer(std::make_shared<PromptRenderer>());
    llm->register_adapter("test", std::make_shared<ServerTestAdapter>(std::move(reply)));
    llm->set_default_adapter("test");
    AgentExecutionProfile profile;
    profile.config.name = "server-test";
    profile.config.system_prompt = "test";
    profile.config.max_iterations = 2;
    profile.deps = {llm, std::make_shared<ToolBus>(), nullptr};
    server.set_execution_profile(std::move(profile));
    server.set_session_store(std::make_shared<InMemorySessionStore>());
}

inline bool rendered_contains(const RenderedPrompt& rendered, std::string_view needle) {
    for (const auto& message : rendered.messages) {
        if (message.value("content", "").find(needle) != std::string::npos) return true;
    }
    return false;
}

} // namespace agent_framework::test

#endif
