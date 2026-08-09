#include <agent/graph_executor/graph_executor.hpp>
#include <agent/internal/agent_thread_state.hpp>
#include <agent/llm_client/llm_client.hpp>
#include <agent/prompt_renderer/prompt_renderer.hpp>
#include <agent/toolbus/toolbus.hpp>

#include <future>
#include <stdexcept>
#include <vector>

#include <taskflow/taskflow.hpp>

using namespace agent_framework;

namespace {
void require(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
}

class FinalAdapter final : public ModelAdapter {
public:
    std::future<LLMOutput> invoke(const LLMInput&,
                                 std::function<void(std::string_view)> = nullptr) override {
        throw std::logic_error("renderer bypassed");
    }
    std::future<LLMOutput> invoke_with_rendered(
        const RenderedPrompt&, std::function<void(std::string_view)> = nullptr) override {
        std::promise<LLMOutput> promise;
        LLMOutput output;
        if(calls++ == 0) {
            output.is_final = false;
            output.tool_calls.push_back(
                CallSpec{"assembly_probe", json::object(), std::string("call-assembly")});
        } else {
            output.is_final = true;
            output.final_answer = "ok";
        }
        promise.set_value(std::move(output));
        return promise.get_future();
    }
    std::vector<ToolMeta> get_available_tools() const override { return {}; }
    void configure(const ModelConfig&) override {}
    std::string get_model_name() const override { return "assembly-events"; }
    bool supports_multimodal() const override { return false; }
    int calls = 0;
};
}

int main() {
    auto llm = std::make_shared<LLMClient>();
    llm->set_prompt_renderer(std::make_shared<PromptRenderer>());
    llm->register_adapter("test", std::make_shared<FinalAdapter>());
    llm->set_default_adapter("test");

    ExecutionRequest request;
    request.config.system_prompt = "system";
    request.config.max_iterations = 2;
    request.config.extra_config["BUDGET_MAX_COMBINED_PROMPT_ATTACH_BYTES"] = 64;
    request.config.extra_config["BUDGET_MAX_TOOL_RESULT_JSON_BYTES"] = 24;
    auto toolbus = std::make_shared<ToolBus>();
    ToolMeta tool_meta;
    tool_meta.name = "assembly_probe";
    tool_meta.description = "deterministic assembly fixture";
    tool_meta.schema = {{"type", "object"}, {"properties", json::object()}};
    tool_meta.side_effect = ToolSideEffect::ReadOnly;
    toolbus->register_local_tool("assembly_probe",
                                 [](const json&) { return json{{"ok", true}}; },
                                 tool_meta);
    request.deps = {llm, toolbus, nullptr};
    request.session = std::make_shared<internal::AgentThreadState>();
    request.session->initial_user_prompt = "task";
    Message tool;
    tool.role = "tool";
    tool.content = "SECRET_TOOL_CONTEXT_THAT_MUST_NOT_ENTER_EVENTS";
    tool.tool_call_id = "call-1";
    tool.tool_name = "fixture";
    request.session->history.push_back(tool);
    request.context.session_id = "assembly-events-session";
    request.options.persist_session = false;
    request.options.input_already_processed = true;
    request.options.react.sink.on_final_json = [](const json&) {};
    std::vector<ExecutionEvent> events;
    request.event_sink = [&](const ExecutionEvent& event) { events.push_back(event); };

    tf::Executor executor(2);
    GraphExecutor graph_executor;
    const auto result = graph_executor.execute_sync(executor, std::move(request));
    require(result.success, "execution failed");
    std::size_t assembled = 0;
    bool evicted = false;
    for(const auto& event : events) {
        if(event.type == ExecutionEventType::MemoryAssembled) ++assembled;
        evicted = evicted || event.type == ExecutionEventType::MemoryEvicted;
        const auto payload = event.payload.dump();
        require(payload.find("SECRET_TOOL_CONTEXT") == std::string::npos,
                "raw memory text leaked into event");
        require(payload.find("history-0") == std::string::npos,
                "raw memory source id leaked into event");
    }
    require(assembled == 2, "MemoryAssembled was not emitted once per iteration");
    require(evicted, "MemoryEvicted was not emitted");
}
