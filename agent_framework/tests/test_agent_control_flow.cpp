#include <agent/graph_executor.hpp>
#include <agent/internal/agent_thread_state.hpp>
#include <agent/llm_client.hpp>
#include <agent/prompt_renderer.hpp>
#include <agent/task_state_machine.hpp>
#include <agent/toolbus.hpp>

#include <cassert>
#include <future>
#include <memory>
#include <mutex>
#include <string>

namespace {

using namespace agent_framework;

class ThreeToolRoundsAdapter final : public ModelAdapter {
public:
    std::future<LLMOutput> invoke(
        const LLMInput&,
        std::function<void(std::string_view)> = nullptr) override {
        return make_output();
    }

    std::future<LLMOutput> invoke_with_rendered(
        const RenderedPrompt&,
        std::function<void(std::string_view)> = nullptr) override {
        return make_output();
    }

    std::vector<ToolMeta> get_available_tools() const override { return {}; }
    void configure(const ModelConfig&) override {}
    std::string get_model_name() const override { return "three-tool-rounds"; }
    bool supports_multimodal() const override { return false; }
    int calls() const { std::lock_guard<std::mutex> lock(mutex_); return calls_; }

private:
    std::future<LLMOutput> make_output() {
        return std::async(std::launch::async, [this]() {
            std::lock_guard<std::mutex> lock(mutex_);
            ++calls_;
            LLMOutput output;
            if (calls_ <= 3) {
                CallSpec call;
                call.name = "step";
                call.arguments = nlohmann::json{{"round", calls_}};
                call.tool_call_id = "call-" + std::to_string(calls_);
                output.tool_calls.push_back(std::move(call));
            } else {
                output.is_final = true;
                output.final_answer = "finished";
            }
            return output;
        });
    }

    mutable std::mutex mutex_;
    int calls_ = 0;
};

struct AgentFixture {
    std::shared_ptr<ThreeToolRoundsAdapter> adapter;
    std::shared_ptr<LLMClient> llm;
    std::shared_ptr<ToolBus> bus;
    AgentWorkflowDeps deps;
    AgentConfig config;
};

AgentFixture make_fixture() {
    AgentFixture fixture;
    fixture.adapter = std::make_shared<ThreeToolRoundsAdapter>();
    fixture.llm = std::make_shared<LLMClient>();
    fixture.llm->set_prompt_renderer(std::make_shared<PromptRenderer>());
    fixture.llm->register_adapter("fake", fixture.adapter);
    fixture.llm->set_default_adapter("fake");
    fixture.bus = std::make_shared<ToolBus>();
    ToolMeta meta;
    meta.name = "step";
    meta.description = "deterministic test step";
    meta.schema = nlohmann::json::parse(
        R"({"type":"object","properties":{"round":{"type":"integer"}},"required":["round"]})");
    fixture.bus->register_local_tool(
        "step", [](const nlohmann::json& args) {
            return nlohmann::json{{"round", args.at("round")}, {"ok", true}};
        }, meta);
    fixture.deps.llm = fixture.llm;
    fixture.deps.toolbus = fixture.bus;
    fixture.config.system_prompt = "test";
    fixture.config.max_iterations = 8;
    fixture.config.max_tool_calls_per_iteration = 4;
    return fixture;
}

void assert_session(const std::shared_ptr<internal::AgentThreadState>& state) {
    assert(state);
    assert(state->iteration == 4);
    int tool_messages = 0;
    for (const auto& message : state->history) {
        if (message.role == "tool") {
            ++tool_messages;
            assert(message.tool_call_id.has_value());
        }
    }
    assert(tool_messages == 3);
}

void test_three_rounds_and_parallel_sessions() {
    tf::Executor executor(8);
    auto first = make_fixture();
    auto second = make_fixture();
    workflow::GraphBuilder first_graph("first_agent");
    workflow::GraphBuilder second_graph("second_agent");
    auto first_state = std::make_shared<internal::AgentThreadState>();
    auto second_state = std::make_shared<internal::AgentThreadState>();
    first_state->initial_user_prompt = "first";
    second_state->initial_user_prompt = "second";

    std::shared_ptr<internal::AgentThreadState> first_final;
    std::shared_ptr<internal::AgentThreadState> second_final;
    CliAgentTerminalSinkOptions first_sink;
    first_sink.on_final_json = [](const nlohmann::json& value) {
        assert(value.at("final_answer") == "finished");
    };
    first_sink.on_final_state = [&](auto value) { first_final = std::move(value); };
    CliAgentTerminalSinkOptions second_sink = first_sink;
    second_sink.on_final_state = [&](auto value) { second_final = std::move(value); };

    build_cli_agent_graph_with_terminal_sink(
        first_graph, first.config, first.deps, first_state, first_sink);
    build_cli_agent_graph_with_terminal_sink(
        second_graph, second.config, second.deps, second_state, second_sink);
    auto first_run = first_graph.run_async(executor);
    auto second_run = second_graph.run_async(executor);
    first_run.get();
    second_run.get();

    assert(first.adapter->calls() == 4);
    assert(second.adapter->calls() == 4);
    assert_session(first_final);
    assert_session(second_final);
    assert(first_final != second_final);
}

void test_cancelled_session() {
    tf::Executor executor(2);
    auto fixture = make_fixture();
    workflow::GraphBuilder graph("cancelled_agent");
    auto state = std::make_shared<internal::AgentThreadState>();
    state->initial_user_prompt = "cancel";
    auto control = std::make_shared<TaskControl>();
    control->request_cancel();
    CliAgentGraphOptions options;
    options.task_control = control;
    std::string final_answer;
    CliAgentTerminalSinkOptions sink;
    sink.on_final_json = [&](const nlohmann::json& value) {
        final_answer = value.at("final_answer").get<std::string>();
    };
    build_cli_agent_graph_with_terminal_sink(
        graph, fixture.config, fixture.deps, state, sink, "AgentLoop", options);
    graph.run(executor);
    assert(final_answer == "[task] cancelled");
    assert(fixture.adapter->calls() == 0);
}

class ResumeAdapter final : public ModelAdapter {
public:
    std::future<LLMOutput> invoke(
        const LLMInput&, std::function<void(std::string_view)> = nullptr) override {
        return output();
    }
    std::future<LLMOutput> invoke_with_rendered(
        const RenderedPrompt&, std::function<void(std::string_view)> = nullptr) override {
        return output();
    }
    std::vector<ToolMeta> get_available_tools() const override { return {}; }
    void configure(const ModelConfig&) override {}
    std::string get_model_name() const override { return "resume"; }
    bool supports_multimodal() const override { return false; }
private:
    std::future<LLMOutput> output() {
        return std::async(std::launch::async, [this]() {
            LLMOutput result;
            if (calls_++ == 0) {
                CallSpec call;
                call.name = "side_effect";
                call.tool_call_id = "already-committed";
                call.arguments = nlohmann::json::object();
                result.tool_calls.push_back(std::move(call));
            } else {
                result.is_final = true;
                result.final_answer = "resumed";
            }
            return result;
        });
    }
    std::atomic_int calls_ {0};
};

void test_resume_does_not_replay_tool_call() {
    tf::Executor executor(2);
    auto adapter = std::make_shared<ResumeAdapter>();
    auto llm = std::make_shared<LLMClient>();
    llm->set_prompt_renderer(std::make_shared<PromptRenderer>());
    llm->register_adapter("resume", adapter);
    llm->set_default_adapter("resume");
    auto bus = std::make_shared<ToolBus>();
    std::atomic_int tool_calls {0};
    ToolMeta meta;
    meta.name = "side_effect";
    meta.description = "must not be replayed";
    meta.schema = nlohmann::json::parse(R"({"type":"object"})");
    bus->register_local_tool("side_effect", [&](const nlohmann::json&) {
        ++tool_calls;
        return nlohmann::json{{"ok", true}};
    }, meta);

    auto state = std::make_shared<internal::AgentThreadState>();
    state->initial_user_prompt = "resume";
    Message committed;
    committed.role = "tool";
    committed.tool_call_id = "already-committed";
    committed.tool_name = "side_effect";
    committed.tool_result = nlohmann::json{{"ok", true}};
    state->history.push_back(std::move(committed));

    AgentWorkflowDeps deps;
    deps.llm = llm;
    deps.toolbus = bus;
    AgentConfig config;
    config.system_prompt = "test";
    config.max_iterations = 4;
    workflow::GraphBuilder graph("resume_agent");
    std::string final_answer;
    CliAgentTerminalSinkOptions sink;
    sink.on_final_json = [&](const nlohmann::json& value) {
        final_answer = value.at("final_answer").get<std::string>();
    };
    build_cli_agent_graph_with_terminal_sink(graph, config, deps, state, sink);
    graph.run(executor);
    assert(tool_calls == 0);
    assert(final_answer == "resumed");
}

}  // namespace

int main() {
    test_three_rounds_and_parallel_sessions();
    test_cancelled_session();
    test_resume_does_not_replay_tool_call();
}
