#include <agent/agent_client/agent_client.hpp>
#include <agent/agent_server/agent_server.hpp>
#include <agent/llm_client/llm_client.hpp>
#include <agent/prompt_renderer/prompt_renderer.hpp>
#include <agent/toolbus/toolbus.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <future>
#include <thread>

using namespace agent_framework;

namespace {
class RestartAdapter final : public ModelAdapter {
public:
    std::future<LLMOutput> invoke(const LLMInput& input,
                                 std::function<void(std::string_view)> = nullptr) override {
        bool restored = false;
        for (const auto& message : input.history) {
            restored = restored || (message.role == "user" && message.content == "first");
        }
        return ready(restored ? "restored" : "first-answer");
    }
    std::future<LLMOutput> invoke_with_rendered(
        const RenderedPrompt& prompt, std::function<void(std::string_view)> = nullptr) override {
        bool second_turn = false;
        for (const auto& message : prompt.messages) {
            second_turn = second_turn || (message.value("role", "") == "user" &&
                                          message.value("content", "") == "second");
        }
        return ready(second_turn ? "restored" : "first-answer");
    }
    std::vector<ToolMeta> get_available_tools() const override { return {}; }
    void configure(const ModelConfig&) override {}
    std::string get_model_name() const override { return "restart-adapter"; }
    bool supports_multimodal() const override { return false; }
private:
    static std::future<LLMOutput> ready(std::string answer) {
        std::promise<LLMOutput> promise;
        LLMOutput output;
        output.is_final = true;
        output.final_answer = std::move(answer);
        promise.set_value(std::move(output));
        return promise.get_future();
    }
};

AgentExecutionProfile profile() {
    auto llm = std::make_shared<LLMClient>();
    llm->set_prompt_renderer(std::make_shared<PromptRenderer>());
    llm->register_adapter("restart", std::make_shared<RestartAdapter>());
    llm->set_default_adapter("restart");
    AgentExecutionProfile value;
    value.config.system_prompt = "restart";
    value.config.max_iterations = 2;
    value.deps = {llm, std::make_shared<ToolBus>(), nullptr};
    return value;
}

AgentMessage message(std::string text) {
    AgentMessage result;
    result.role = AgentMessage::Role::USER;
    result.timestamp = std::chrono::system_clock::now();
    AgentPart part;
    part.type = AgentPart::Type::TEXT;
    part.text = std::move(text);
    result.parts.push_back(std::move(part));
    return result;
}

bool wait_bound(AgentServer& server) {
    for (int i = 0; i < 1000; ++i) {
        if (server.bound_port() > 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

AgentTask wait_done(AgentClient& client, AgentTask task) {
    for (int i = 0; i < 500 && task.status != AgentTaskStatus::COMPLETED; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        task = client.get_task("", task.task_id).get();
    }
    return task;
}

void run_turn(const std::string& db, std::string prompt, std::string expected) {
    AgentServer server(0);
    server.set_execution_profile(profile());
    server.set_session_store(std::make_shared<SQLiteSessionStore>(db));
    AgentCard card;
    card.name = "restart";
    card.provider = "test";
    card.api_endpoint = "http://127.0.0.1/rpc";
    server.register_agent_card(card);
    std::thread thread([&] { server.start(); });
    assert(wait_bound(server));
    AgentClientOptions options;
    options.use_legacy_rest = false;
    options.json_rpc_path = "/rpc";
    AgentClient client("http://127.0.0.1:" + std::to_string(server.bound_port()), options);
    auto task = wait_done(client, client.send_task("", message(std::move(prompt)), "restart-context").get());
    assert(task.status == AgentTaskStatus::COMPLETED);
    assert(task.messages.back().parts.front().text == expected);
    server.stop();
    thread.join();
}
} // namespace

int main() {
    (void)::setenv("AGENT_SERVER_BIND", "127.0.0.1", 1);
    (void)::setenv("AGENT_SERVER_JSON_RPC_PATH", "/rpc", 1);
    (void)::setenv("AGENT_SERVER_LEGACY_REST", "0", 1);
    (void)::setenv("AGENT_VERIFIER", "off", 1);
    const auto dir = std::filesystem::temp_directory_path() / "agent_phase2_restart";
    std::filesystem::remove_all(dir);
    const std::string db = (dir / "session.sqlite").string();
    run_turn(db, "first", "first-answer");
    run_turn(db, "second", "restored");
    SQLiteSessionStore store(db);
    assert(store.load_or_create("restart-context").revision == 2);
    std::filesystem::remove_all(dir);
}
