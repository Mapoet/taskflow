/**
 * @file test_empty_delivery.cpp
 * @brief Empty EndTurn delivery: stream merge, retry, receipts, no code leak
 */
#include <agent/conversation/graph_turn_adapter.hpp>
#include <agent/conversation/harness_turn_adapter.hpp>
#include <agent/conversation/types.hpp>
#include <agent/graph_executor/graph_executor.hpp>
#include <agent/harness/store.hpp>
#include <agent/internal/agent_thread_state.hpp>
#include <agent/internal/platform_io.hpp>
#include <agent/llm_client/llm_client.hpp>
#include <agent/prompt_renderer/prompt_renderer.hpp>
#include <agent/toolbus/toolbus.hpp>

#include <cassert>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>

#include <taskflow/taskflow.hpp>

namespace {

using namespace agent_framework;

class ScriptedAdapter final : public ModelAdapter {
public:
    enum class Mode { StreamOnly, AlwaysEmpty, EmptyThenAnswer, ToolThenAnswer };

    explicit ScriptedAdapter(Mode mode) : mode_(mode) {}

    std::future<LLMOutput> invoke(
        const LLMInput&,
        std::function<void(std::string_view)> stream = nullptr) override {
        return make(std::move(stream));
    }
    std::future<LLMOutput> invoke_with_rendered(
        const RenderedPrompt&,
        std::function<void(std::string_view)> stream = nullptr) override {
        return make(std::move(stream));
    }
    std::vector<ToolMeta> get_available_tools() const override { return {}; }
    void configure(const ModelConfig&) override {}
    std::string get_model_name() const override { return "empty-delivery"; }
    bool supports_multimodal() const override { return false; }
    int calls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return calls_;
    }

private:
    std::future<LLMOutput> make(std::function<void(std::string_view)> stream) {
        return std::async(std::launch::async, [this, stream]() {
            std::lock_guard<std::mutex> lock(mutex_);
            ++calls_;
            LLMOutput out;
            out.is_final = true;
            if (mode_ == Mode::StreamOnly) {
                if (stream) stream("streamed-body");
                return out;
            }
            if (mode_ == Mode::AlwaysEmpty)
                return out;
            if (mode_ == Mode::EmptyThenAnswer) {
                if (calls_ >= 2)
                    out.final_answer = "recovered";
                return out;
            }
            if (calls_ == 1) {
                out.is_final = false;
                CallSpec call;
                call.name = "noop";
                call.arguments = nlohmann::json::object();
                call.tool_call_id = "receipt-noop-1";
                out.tool_calls.push_back(std::move(call));
                return out;
            }
            out.final_answer = "after-tool";
            return out;
        });
    }

    Mode mode_;
    mutable std::mutex mutex_;
    int calls_{0};
};

struct Fixture {
    std::shared_ptr<ScriptedAdapter> adapter;
    std::shared_ptr<LLMClient> llm;
    std::shared_ptr<ToolBus> bus;
    AgentWorkflowDeps deps;
    AgentConfig config;
};

Fixture make_fixture(ScriptedAdapter::Mode mode) {
    Fixture fixture;
    fixture.adapter = std::make_shared<ScriptedAdapter>(mode);
    fixture.llm = std::make_shared<LLMClient>();
    fixture.llm->set_prompt_renderer(std::make_shared<PromptRenderer>());
    fixture.llm->register_adapter("fake", fixture.adapter);
    fixture.llm->set_default_adapter("fake");
    fixture.bus = std::make_shared<ToolBus>();
    ToolMeta meta;
    meta.name = "noop";
    meta.description = "noop";
    meta.schema = nlohmann::json::parse(R"({"type":"object","properties":{}})");
    fixture.bus->register_local_tool(
        "noop", [](const nlohmann::json&) { return nlohmann::json{{"ok", true}}; }, meta);
    fixture.deps.llm = fixture.llm;
    fixture.deps.toolbus = fixture.bus;
    fixture.config.system_prompt = "test";
    fixture.config.max_iterations = 4;
    fixture.config.max_tool_calls_per_iteration = 2;
    return fixture;
}

WorkflowResult run_sync(Fixture& fixture, const std::string& prompt) {
    tf::Executor executor(2);
    GraphExecutor gx;
    ReactCliRunRequest req;
    req.config = fixture.config;
    req.deps = fixture.deps;
    req.session = std::make_shared<internal::AgentThreadState>();
    req.session->initial_user_prompt = prompt;
    req.options.sink.on_final_json = [](const nlohmann::json&) {};
    return gx.run_react_cli_sync(executor, req);
}

void test_stream_merge() {
    auto fixture = make_fixture(ScriptedAdapter::Mode::StreamOnly);
    const auto wr = run_sync(fixture, "stream");
    assert(wr.success);
    assert(wr.outputs.at("final_answer") == "streamed-body");
    assert(wr.outputs.value("model_stop_reason", "") == "model_turn_completed");
    assert(fixture.adapter->calls() == 1);
}

void test_empty_retry_then_fail() {
    auto fixture = make_fixture(ScriptedAdapter::Mode::AlwaysEmpty);
    const auto wr = run_sync(fixture, "empty");
    assert(!wr.success);
    assert(wr.error_message.has_value());
    assert(wr.error_message->find("interactive_execution") == std::string::npos);
    assert(wr.outputs.value("model_stop_reason", "") == "empty_delivery");
    assert(wr.outputs.value("final_answer", "x").empty());
    assert(fixture.adapter->calls() == 2);
    auto turn = conversation::GraphTurnAdapter::from_workflow(wr);
    assert(turn.reason == conversation::ModelTurnStopReason::ProviderError);
    assert(turn.candidate_answer.empty());
}

void test_empty_then_recover() {
    auto fixture = make_fixture(ScriptedAdapter::Mode::EmptyThenAnswer);
    const auto wr = run_sync(fixture, "retry");
    assert(wr.success);
    assert(wr.outputs.at("final_answer") == "recovered");
    assert(fixture.adapter->calls() == 2);
}

void test_tool_receipts_are_delivery() {
    auto fixture = make_fixture(ScriptedAdapter::Mode::ToolThenAnswer);
    const auto wr = run_sync(fixture, "tool");
    assert(wr.success);
    assert(wr.outputs.at("final_answer") == "after-tool");
    assert(wr.outputs.contains("tool_receipt_refs"));
    assert(wr.outputs["tool_receipt_refs"].is_array());
    assert(wr.outputs["tool_receipt_refs"].size() == 1);
    assert(wr.outputs["tool_receipt_refs"][0] == "receipt-noop-1");
    auto turn = conversation::GraphTurnAdapter::from_workflow(wr);
    assert(turn.reason == conversation::ModelTurnStopReason::EndTurn);
    assert(turn.tool_receipt_refs.size() == 1);
}

void test_prior_turn_receipts_do_not_mask_empty_delivery() {
    auto fixture = make_fixture(ScriptedAdapter::Mode::AlwaysEmpty);
    tf::Executor executor(2);
    GraphExecutor gx;
    ReactCliRunRequest req;
    req.config = fixture.config;
    req.deps = fixture.deps;
    req.session = std::make_shared<internal::AgentThreadState>();
    Message prior;
    prior.role = "tool";
    prior.content = R"({"ok":true})";
    prior.tool_call_id = "prior-turn-receipt";
    req.session->history.push_back(std::move(prior));
    req.session->initial_user_prompt = "empty after prior tool";
    req.options.sink.on_final_json = [](const nlohmann::json&) {};
    const auto wr = gx.run_react_cli_sync(executor, req);
    assert(!wr.success);
    assert(wr.outputs.contains("tool_receipt_refs"));
    assert(wr.outputs["tool_receipt_refs"].empty());
    assert(wr.outputs.value("model_stop_reason", "") == "empty_delivery");
}

void test_harness_does_not_leak_code() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("empty-delivery-" + std::to_string(internal::current_process_id()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    harness::SQLiteHarnessStore store((root / "harness.sqlite3").string());
    conversation::HarnessTurnAdapter adapter(store, [](const auto&) {
        conversation::ModelTurnOutcome out;
        out.reason = conversation::ModelTurnStopReason::EndTurn;
        return out;
    });
    conversation::HarnessSupportedTurnRequest request;
    request.turn.identity = {"tenant", "conversation"};
    request.turn.turn_id = "turn-empty";
    request.turn.input = "hello";
    const auto outcome = adapter.execute(request);
    assert(outcome.reason == conversation::ModelTurnStopReason::GuardStopped);
    assert(outcome.candidate_answer.empty());
    assert(conversation::user_facing_turn_failure("interactive_execution_empty_delivery")
               .find('_') == std::string::npos);
    fs::remove_all(root, ec);
}

} // namespace

int main() {
    test_stream_merge();
    test_empty_retry_then_fail();
    test_empty_then_recover();
    test_tool_receipts_are_delivery();
    test_prior_turn_receipts_do_not_mask_empty_delivery();
    test_harness_does_not_leak_code();
    return 0;
}
