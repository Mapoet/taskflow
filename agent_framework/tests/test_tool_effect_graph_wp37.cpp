#include <agent/graph_executor/graph_executor.hpp>
#include <agent/internal/agent_thread_state.hpp>
#include <agent/llm_client/llm_client.hpp>
#include <agent/observability/audit.hpp>
#include <agent/prompt_renderer/prompt_renderer.hpp>
#include <agent/toolbus/tool_effect_journal.hpp>
#include <agent/toolbus/toolbus.hpp>

#include <taskflow/taskflow.hpp>

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <future>

using namespace agent_framework;

namespace {
class WriteAdapter final : public ModelAdapter {
public:
    explicit WriteAdapter(bool include_key) : include_key_(include_key) {}
    std::future<LLMOutput> invoke(const LLMInput&,
        std::function<void(std::string_view)> = nullptr) override {
        throw std::logic_error("rendered prompt required");
    }
    std::future<LLMOutput> invoke_with_rendered(const RenderedPrompt&,
        std::function<void(std::string_view)> = nullptr) override {
        std::promise<LLMOutput> promise;
        LLMOutput output;
        if (calls_++ == 0) {
            output.is_final = false;
            json args = {{"value", 7}, {"token", "do-not-log-this-token"}};
            if (include_key_) args["idempotency_key"] = "effect-1";
            output.tool_calls.push_back(CallSpec{"write_once", std::move(args), "call-write-1"});
        } else {
            output.is_final = true;
            output.final_answer = "done";
        }
        promise.set_value(std::move(output));
        return promise.get_future();
    }
    std::vector<ToolMeta> get_available_tools() const override { return {}; }
    void configure(const ModelConfig&) override {}
    std::string get_model_name() const override { return "effect-graph-fixture"; }
    bool supports_multimodal() const override { return false; }
private:
    bool include_key_;
    int calls_{0};
};

ExecutionResult run_once(GraphExecutor& graph, tf::Executor& executor,
                         const std::shared_ptr<ToolBus>& bus,
                         const std::shared_ptr<ToolEffectJournal>& journal,
                         const std::shared_ptr<SessionStore>& sessions,
                         const std::string& session_id, bool include_key,
                         std::shared_ptr<AuditSink> audit = {}) {
    auto llm = std::make_shared<LLMClient>();
    llm->set_prompt_renderer(std::make_shared<PromptRenderer>());
    llm->register_adapter("fixture", std::make_shared<WriteAdapter>(include_key));
    llm->set_default_adapter("fixture");
    ExecutionRequest request;
    request.config.system_prompt = "system";
    request.config.max_iterations = 3;
    request.deps = {llm, bus, nullptr};
    request.session = std::make_shared<internal::AgentThreadState>();
    request.session->initial_user_prompt = "perform write";
    request.context.session_id = session_id;
    request.context.task_id = "effect-task";
    request.context.trace_id = "trace-" + session_id;
    request.session_store = sessions;
    request.tool_effect_journal = journal;
    request.audit_sink = std::move(audit);
    request.options.react.sink.on_final_json = [](const json&) {};
    return graph.execute_sync(executor, std::move(request));
}
} // namespace

int main() {
    (void)::setenv("AGENT_VERIFIER", "off", 1);
    (void)::setenv("AGENT_TOOL_ALLOWLIST", "", 1);
    const auto root = std::filesystem::temp_directory_path() / "agent-effect-graph-wp37";
    std::filesystem::remove_all(root);
    auto journal = std::make_shared<ToolEffectJournal>(root / "effects.jsonl");
    auto sessions = std::make_shared<InMemorySessionStore>();
    auto bus = std::make_shared<ToolBus>();
    std::atomic<int> writes{0};
    ToolMeta meta;
    meta.name = "write_once";
    meta.description = "write fixture";
    meta.side_effect = ToolSideEffect::Write;
    meta.schema = {{"type", "object"}, {"properties", {
        {"value", {{"type", "integer"}}},
        {"token", {{"type", "string"}}},
        {"idempotency_key", {{"type", "string"}}}}},
        {"required", {"value"}}};
    bus->register_local_tool("write_once", [&writes](const json&) {
        return json{{"write_number", ++writes}};
    }, meta);

    tf::Executor executor(2);
    GraphExecutor graph;
    auto audit = std::make_shared<TestAuditSink>();
    const auto first = run_once(graph, executor, bus, journal, sessions, "session-a", true, audit);
    assert(first.success && first.committed_revision == 1);
    assert(writes.load() == 1);
    const auto committed = journal->find_idempotency("session-a:effect-1");
    assert(committed && committed->status == ToolEffectStatus::Committed);
    const auto trace = audit->events_for_trace("trace-session-a");
    assert(!trace.empty());
    std::uint64_t prior_sequence = 0;
    bool saw_tool = false, saw_checkpoint = false, saw_completion = false;
    for (const auto& event : trace) {
        assert(event.sequence > prior_sequence);
        prior_sequence = event.sequence;
        saw_tool = saw_tool || event.component == "tool";
        saw_checkpoint = saw_checkpoint || event.event_kind == "checkpoint_committed";
        saw_completion = saw_completion || event.event_kind == "execution_completed";
        assert(audit_event_to_json(event).dump().find("do-not-log-this-token") == std::string::npos);
    }
    assert(saw_tool && saw_checkpoint && saw_completion);

    // A repeated write with the same session-scoped key is rejected before dispatch.
    const auto duplicate = run_once(graph, executor, bus, journal, sessions, "session-a", true);
    // The ReAct loop may recover from the structured tool error and still produce a final answer;
    // the exactly-once invariant is that the external write is never dispatched again.
    assert(duplicate.success);
    assert(writes.load() == 1);

    // A write without an explicit key is quarantined for manual review before dispatch.
    const auto missing_key = run_once(graph, executor, bus, journal, sessions, "session-b", false);
    assert(!missing_key.success);
    assert(writes.load() == 1);
    const auto records = journal->recoverable();
    bool found_manual = false;
    for (const auto& item : records) {
        if (item.session_id == "session-b" && item.status == ToolEffectStatus::ManualReview)
            found_manual = true;
    }
    assert(found_manual);
    std::filesystem::remove_all(root);
}
