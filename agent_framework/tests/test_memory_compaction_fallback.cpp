/**
 * @file test_memory_compaction_fallback.cpp
 * @brief WP2.9 F-1（summarize 失败回退）, F-2（硬上限后 1c 包装）
 */

#include <agent/context_budget/context_budget.hpp>
#include <agent/internal/agent_thread_state.hpp>
#include <agent/llm_client/llm_client.hpp>
#include <agent/agent/memory_compaction.hpp>
#include <agent/prompt_renderer/prompt_renderer.hpp>
#include <agent/core/types.hpp>
#include <agent/agent/working_memory_metrics.hpp>

#include <cassert>
#include <cstdlib>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include <stdlib.h>
#else
#include <unistd.h>
#endif

namespace {

using namespace agent_framework;

void set_env(const char* k, const char* v) {
#if defined(_WIN32)
    (void)_putenv_s(k, v);
#else
    (void)::setenv(k, v, 1);
#endif
}

void unset_env(const char* k) {
#if defined(_WIN32)
    (void)_putenv_s(k, "");
#else
    (void)::unsetenv(k);
#endif
}

class ThrowOnInvokeAdapter final : public ModelAdapter {
public:
    std::future<LLMOutput> invoke(const LLMInput& /*input*/,
                                  std::function<void(std::string_view)> /*cb*/) override {
        return std::async(std::launch::deferred, []() -> LLMOutput {
            throw std::runtime_error("mock llm failure");
        });
    }

    std::future<LLMOutput> invoke_with_rendered(
        const RenderedPrompt& /*rendered*/,
        std::function<void(std::string_view)> /*cb*/) override {
        return std::async(std::launch::deferred, []() -> LLMOutput {
            throw std::runtime_error("mock llm failure");
        });
    }

    std::vector<ToolMeta> get_available_tools() const override {
        return {};
    }

    void configure(const ModelConfig& /*config*/) override {}

    std::string get_model_name() const override {
        return "throw-mock";
    }

    bool supports_multimodal() const override {
        return false;
    }
};

void test_f1_summarize_fallback_truncate() {
    unset_env("AGENT_MEMORY_COMPACT_MODE");
    set_env("AGENT_MEMORY_COMPACT_MODE", "summarize");
    set_env("AGENT_MEMORY_COMPACT_HEAD_KEEP", "1");
    set_env("AGENT_MEMORY_COMPACT_TAIL_KEEP", "8");

    auto llm = std::make_shared<LLMClient>();
    llm->set_prompt_renderer(std::make_shared<PromptRenderer>());
    llm->register_adapter("x", std::make_shared<ThrowOnInvokeAdapter>());
    llm->set_default_adapter("x");

    internal::AgentThreadState st;
    for (int i = 0; i < 20; ++i) {
        Message m;
        m.role = (i % 2 == 0) ? "user" : "assistant";
        m.content = "line-" + std::to_string(i);
        st.history.push_back(std::move(m));
    }

    MemoryCompactOptions opt;
    opt.llm_client = llm.get();
    const MemoryCompactResult r =
        run_memory_compaction(st, MemoryCompactTrigger::manual_compact, opt);
    assert(r.did_mutate);
    assert(r.strategy_used == "fallback_truncate");
    assert(st.history.size() == 10);
    assert(st.history[1].role == "system");
}

void test_f2_hard_cap_uses_af_truncation_on_tool() {
    unset_env("AGENT_MEMORY_COMPACT_MODE");
    set_env("AGENT_MEMORY_COMPACT_HEAD_KEEP", "1");
    set_env("AGENT_MEMORY_COMPACT_TAIL_KEEP", "1");
    set_env("AGENT_MEMORY_HARD_LIMIT_BYTES", "2500");
    set_env("AGENT_BUDGET_MAX_TOOL_RESULT_JSON_BYTES", "512");

    internal::AgentThreadState st;
    for (int i = 0; i < 4; ++i) {
        Message m;
        m.role = (i % 2 == 0) ? "user" : "assistant";
        m.content = "u";
        st.history.push_back(std::move(m));
    }
    Message toolm;
    toolm.role = "tool";
    toolm.tool_result = json{{"blob", std::string(8000, 'b')}};
    st.history.push_back(std::move(toolm));

    AgentConfig cfg;
    MemoryCompactOptions opt;
    opt.agent_config = &cfg;

    const MemoryCompactResult r =
        run_memory_compaction(st, MemoryCompactTrigger::manual_compact, opt);
    assert(r.did_mutate);
    bool found_trunc = false;
    for (const auto& m : st.history) {
        if (m.role == "tool" && m.tool_result && m.tool_result->contains("_af_truncation")) {
            found_trunc = true;
            break;
        }
    }
    assert(found_trunc);
    assert(history_utf8_bytes_total(st) <= 2500u);
}

} // namespace

int main() {
    test_f1_summarize_fallback_truncate();
    test_f2_hard_cap_uses_af_truncation_on_tool();
    return 0;
}
