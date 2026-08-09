#include <agent/agent/memory_compaction.hpp>
#include <agent/internal/agent_thread_state.hpp>
#include <agent/llm_client/llm_client.hpp>
#include <agent/prompt_renderer/prompt_renderer.hpp>

#include <cstdlib>
#include <future>
#include <stdexcept>

using namespace agent_framework;

namespace {
void require(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
}

void set_env(const char* key, const char* value) {
#if defined(_WIN32)
    (void)_putenv_s(key, value);
#else
    (void)::setenv(key, value, 1);
#endif
}

internal::AgentThreadState history() {
    internal::AgentThreadState state;
    for(int index = 0; index < 8; ++index) {
        Message message;
        message.role = index % 2 ? "assistant" : "user";
        message.content = "fact-" + std::to_string(index);
        state.history.push_back(std::move(message));
    }
    return state;
}

class StructuredAdapter final : public ModelAdapter {
public:
    explicit StructuredAdapter(bool valid = true) : valid_(valid) {}
    std::future<LLMOutput> invoke(const LLMInput&,
                                 std::function<void(std::string_view)> = nullptr) override {
        throw std::logic_error("renderer bypassed");
    }
    std::future<LLMOutput> invoke_with_rendered(
        const RenderedPrompt& rendered,
        std::function<void(std::string_view)> = nullptr) override {
        ++calls;
        require(rendered.tools_json.empty() || rendered.tools_json == json::array(),
                "compaction sub-LLM received tools");
        const std::string wire = rendered.rendered_text + json(rendered.messages).dump();
        const auto marker = wire.find("source_digest=");
        require(marker != std::string::npos, "source digest missing from request");
        const auto digest = wire.substr(marker + 14, 64);
        std::promise<LLMOutput> promise;
        LLMOutput output;
        output.is_final = true;
        output.final_answer = valid_
            ? json{{"schema_version", 1}, {"summary", "stable summary"},
                   {"facts", json::array({"fact-1"})}, {"open_items", json::array()},
                   {"source_digest", digest}}.dump()
            : "{\"summary\":\"missing schema\"}";
        promise.set_value(std::move(output));
        return promise.get_future();
    }
    std::vector<ToolMeta> get_available_tools() const override { return {}; }
    void configure(const ModelConfig&) override {}
    std::string get_model_name() const override { return "structured-compactor"; }
    bool supports_multimodal() const override { return false; }
    int calls = 0;
private:
    bool valid_;
};

std::shared_ptr<LLMClient> client(const std::shared_ptr<ModelAdapter>& adapter) {
    auto result = std::make_shared<LLMClient>();
    result->set_prompt_renderer(std::make_shared<PromptRenderer>());
    result->register_adapter("compact", adapter);
    result->set_default_adapter("compact");
    return result;
}

class ProbeCompactor final : public MemoryCompactor {
public:
    MemoryCompactResult compact(const MemoryCompactionInput& input,
                                const MemoryCompactionContext&) override {
        MemoryCompactResult result;
        result.bytes_before = input.bytes_before;
        result.bytes_after = input.bytes_before;
        result.strategy_used = "probe";
        result.log_reason = "probe_called";
        result.outcome = MemoryCompactionOutcome::NoOp;
        called = true;
        return result;
    }
    bool called = false;
};

class NeverReadyAdapter final : public ModelAdapter {
public:
    std::future<LLMOutput> invoke(const LLMInput&,
                                 std::function<void(std::string_view)> = nullptr) override {
        throw std::logic_error("renderer bypassed");
    }
    std::future<LLMOutput> invoke_with_rendered(
        const RenderedPrompt&, std::function<void(std::string_view)> = nullptr) override {
        ++calls;
        auto promise = std::make_shared<std::promise<LLMOutput>>();
        pending.push_back(promise);
        return promise->get_future();
    }
    std::vector<ToolMeta> get_available_tools() const override { return {}; }
    void configure(const ModelConfig&) override {}
    std::string get_model_name() const override { return "never-ready-compactor"; }
    bool supports_multimodal() const override { return false; }
    int calls = 0;
    std::vector<std::shared_ptr<std::promise<LLMOutput>>> pending;
};
}

int main() {
    set_env("AGENT_MEMORY_COMPACT_HEAD_KEEP", "1");
    set_env("AGENT_MEMORY_COMPACT_TAIL_KEEP", "1");

    auto valid_adapter = std::make_shared<StructuredAdapter>();
    auto valid_client = client(valid_adapter);
    auto state = history();
    MemoryCompactOptions options;
    options.strategy_id = "structured";
    options.sub_llm_client = valid_client.get();
    options.profile.provider = "compact";
    options.profile.timeout_ms = 100;
    options.profile.max_output_bytes = 4096;
    const auto structured = run_memory_compaction(
        state, MemoryCompactTrigger::manual_compact, options);
    require(structured.did_mutate && structured.strategy_used == "structured",
            "structured compactor did not commit");
    require(structured.source_digest.size() == 64, "source digest missing");
    require(state.history[1].content.find("\"schema_version\":1") != std::string::npos,
            "structured schema was not preserved");
    require(state.history[1].content.find(structured.source_digest) != std::string::npos,
            "committed source digest differs");
    require(valid_adapter->calls == 1, "unexpected retry count");

    auto invalid_adapter = std::make_shared<StructuredAdapter>(false);
    auto invalid_client = client(invalid_adapter);
    state = history();
    options.sub_llm_client = invalid_client.get();
    const auto fallback = run_memory_compaction(
        state, MemoryCompactTrigger::manual_compact, options);
    require(fallback.did_mutate && fallback.strategy_used == "fallback_extractive",
            "schema failure did not follow structured->extractive fallback");

    state = history();
    const auto original = state.history;
    options.sub_llm_client = valid_client.get();
    options.cancellation_requested = [] { return true; };
    const auto cancelled = run_memory_compaction(
        state, MemoryCompactTrigger::manual_compact, options);
    require(cancelled.cancelled && !cancelled.did_mutate, "cancellation mutated memory");
    require(state.history.size() == original.size(), "cancellation triggered fallback");
    require(valid_adapter->calls == 1, "cancellation triggered an LLM request");

    auto timeout_adapter = std::make_shared<NeverReadyAdapter>();
    auto timeout_client = client(timeout_adapter);
    state = history();
    options.cancellation_requested = {};
    options.sub_llm_client = timeout_client.get();
    options.profile.timeout_ms = 5;
    options.profile.max_retries = 1;
    const auto timed_out = run_memory_compaction(
        state, MemoryCompactTrigger::manual_compact, options);
    require(timed_out.did_mutate && timed_out.strategy_used == "fallback_extractive",
            "timeout did not follow the deterministic fallback chain");
    require(timeout_adapter->calls == 2, "configured retry count was not honored");

    auto registry = std::make_shared<MemoryCompactorRegistry>();
    auto probe = std::make_shared<ProbeCompactor>();
    registry->register_compactor("probe", probe);
    require(registry->resolve("probe") == probe && !registry->resolve("missing"),
            "registry resolution failed");
    state = history();
    MemoryCompactOptions injected;
    injected.registry = registry;
    injected.strategy_id = "probe";
    const auto probed = run_memory_compaction(
        state, MemoryCompactTrigger::manual_compact, injected);
    require(probe->called && probed.strategy_used == "probe", "injected compactor not called");

    bool unknown = false;
    try {
        injected.strategy_id = "missing";
        (void)run_memory_compaction(state, MemoryCompactTrigger::manual_compact, injected);
    } catch(const std::invalid_argument&) {
        unknown = true;
    }
    require(unknown, "unknown strategy was accepted");
}
