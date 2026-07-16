#include <agent/agent_client/agent_client.hpp>
#include <agent/agent_server/agent_server.hpp>
#include <agent/llm_client/llm_client.hpp>
#include <agent/prompt_renderer/prompt_renderer.hpp>
#include <agent/toolbus/toolbus.hpp>
#include <agent/agent/task_state_machine.hpp>

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

using namespace agent_framework;

namespace {

class PersistentAdapter final : public ModelAdapter {
public:
    std::future<LLMOutput> invoke(const LLMInput& input,
        std::function<void(std::string_view)> = nullptr) override {
        return make(input.history);
    }
    std::future<LLMOutput> invoke_with_rendered(const RenderedPrompt& rendered,
        std::function<void(std::string_view)> = nullptr) override {
        bool saw_first = false;
        for (const auto& m : rendered.messages) {
            saw_first = saw_first || (m.value("role", "") == "user" &&
                                      m.value("content", "") == "first");
        }
        return ready(saw_first);
    }
    std::vector<ToolMeta> get_available_tools() const override { return {}; }
    void configure(const ModelConfig&) override {}
    std::string get_model_name() const override { return "phase2-a2a-mock"; }
    bool supports_multimodal() const override { return false; }

private:
    std::future<LLMOutput> make(const std::vector<Message>& history) {
        bool saw = false;
        for (const auto& m : history) saw = saw || (m.role == "user" && m.content == "first");
        return ready(saw);
    }
    std::future<LLMOutput> ready(bool saw_first) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++calls_;
        LLMOutput out;
        out.is_final = true;
        out.final_answer = calls_ == 1 ? "one" : (saw_first ? "two" : "missing-history");
        std::promise<LLMOutput> p;
        p.set_value(std::move(out));
        return p.get_future();
    }
    std::mutex mutex_;
    int calls_ = 0;
};

AgentMessage user_message(std::string text) {
    AgentMessage m;
    m.role = AgentMessage::Role::USER;
    m.timestamp = std::chrono::system_clock::now();
    AgentPart p;
    p.type = AgentPart::Type::TEXT;
    p.text = std::move(text);
    m.parts.push_back(std::move(p));
    return m;
}

bool wait_bound(AgentServer& server) {
    for (int i = 0; i < 1000; ++i) {
        if (server.bound_port() > 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

AgentTask wait_terminal(AgentClient& client, const AgentTask& initial) {
    AgentTask task = initial;
    auto terminal = [](AgentTaskStatus status) {
        return status == AgentTaskStatus::COMPLETED || status == AgentTaskStatus::FAILED ||
               status == AgentTaskStatus::CANCELLED;
    };
    for (int i = 0; i < 500 && !terminal(task.status); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        task = client.get_task("", task.task_id).get();
    }
    return task;
}

}  // namespace

int main() {
    (void)::setenv("AGENT_SERVER_BIND", "127.0.0.1", 1);
    (void)::setenv("AGENT_SERVER_JSON_RPC_PATH", "/rpc", 1);
    (void)::setenv("AGENT_SERVER_LEGACY_REST", "0", 1);
    (void)::setenv("AGENT_A2A_STRICT", "1", 1);
    (void)::setenv("AGENT_VERIFIER", "off", 1);
    (void)::setenv("AGENT_TOOL_ALLOWLIST", "", 1);

    auto llm = std::make_shared<LLMClient>();
    llm->set_prompt_renderer(std::make_shared<PromptRenderer>());
    llm->register_adapter("mock", std::make_shared<PersistentAdapter>());
    llm->set_default_adapter("mock");
    auto tools = std::make_shared<ToolBus>();
    auto store = std::make_shared<InMemorySessionStore>();

    AgentExecutionProfile profile;
    profile.config.name = "phase2-a2a";
    profile.config.system_prompt = "system";
    profile.config.max_iterations = 2;
    profile.deps = {llm, tools, nullptr};

    AgentServer server(0);
    server.set_execution_profile(profile);
    server.set_session_store(store);
    AgentCard card;
    card.name = "phase2";
    card.description = "phase2 integration";
    card.provider = "test";
    card.api_endpoint = "http://127.0.0.1/rpc";
    server.register_agent_card(card);
    std::thread server_thread([&] { server.start(); });
    assert(wait_bound(server));

    AgentClientOptions options;
    options.use_legacy_rest = false;
    options.json_rpc_path = "/rpc";
    AgentClient client("http://127.0.0.1:" + std::to_string(server.bound_port()), options);

    AgentTask first = wait_terminal(client, client.send_task("", user_message("first"), "ctx-e2e").get());
    assert(first.status == AgentTaskStatus::COMPLETED);
    std::mutex subscribe_mutex;
    std::condition_variable subscribe_cv;
    bool subscribed_snapshot = false;
    client.subscribe_task_updates(
        "", first.task_id,
        [&](const AgentTask& task) {
            if (task.task_id == first.task_id && task.status == AgentTaskStatus::COMPLETED) {
                std::lock_guard<std::mutex> lock(subscribe_mutex);
                subscribed_snapshot = true;
                subscribe_cv.notify_all();
            }
        }, [](const AgentArtifact&) {});
    {
        std::unique_lock<std::mutex> lock(subscribe_mutex);
        assert(subscribe_cv.wait_for(lock, std::chrono::seconds(5), [&] { return subscribed_snapshot; }));
    }
    AgentTask second = wait_terminal(client, client.send_task("", user_message("second"), "ctx-e2e").get());
    assert(second.status == AgentTaskStatus::COMPLETED);
    assert(!second.messages.empty());
    assert(second.messages.back().parts.front().text == "two");
    assert(store->load_or_create("ctx-e2e").revision == 2);

    std::mutex stream_mutex;
    std::condition_variable stream_cv;
    bool stream_completed = false;
    client.send_streaming_task(
        "", user_message("stream"), "ctx-stream", json::object(),
        [&](const AgentTask& task) {
            if (task.status == AgentTaskStatus::COMPLETED) {
                std::lock_guard<std::mutex> lock(stream_mutex);
                stream_completed = true;
                stream_cv.notify_all();
            }
        }, [](const AgentArtifact&) {});
    {
        std::unique_lock<std::mutex> lock(stream_mutex);
        assert(stream_cv.wait_for(lock, std::chrono::seconds(5), [&] { return stream_completed; }));
    }

    server.stop();
    server_thread.join();
}
