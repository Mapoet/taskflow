/**
 * @file test_memory_compaction_truncate.cpp
 * @brief WP2.9 C-1, C-2, C-3
 */

#include <agent/internal/agent_thread_state.hpp>
#include <agent/memory_compaction.hpp>
#include <agent/user_input_preprocessor.hpp>

#include <cassert>
#include <cstdlib>
#include <string>
#include <vector>

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

void fill_compact_mode_truncate() {
    unset_env("AGENT_MEMORY_COMPACT_MODE");
}

internal::AgentThreadState make_linear_history(std::size_t n) {
    internal::AgentThreadState st;
    for (std::size_t i = 0; i < n; ++i) {
        Message m;
        m.role = (i % 2 == 0) ? "user" : "assistant";
        m.content = "m" + std::to_string(i);
        st.history.push_back(std::move(m));
    }
    return st;
}

void test_c1_truncate_shape() {
    fill_compact_mode_truncate();
    set_env("AGENT_MEMORY_COMPACT_HEAD_KEEP", "1");
    set_env("AGENT_MEMORY_COMPACT_TAIL_KEEP", "8");

    internal::AgentThreadState st = make_linear_history(20);
    MemoryCompactOptions opt;
    const MemoryCompactResult r =
        run_memory_compaction(st, MemoryCompactTrigger::manual_compact, opt);
    assert(r.did_mutate);
    assert(st.history.size() == 10);
    assert(st.history[1].role == "system");
    assert(st.history[1].content.find("memory_compacted") != std::string::npos);
}

void test_c2_noop_small_n() {
    fill_compact_mode_truncate();
    set_env("AGENT_MEMORY_COMPACT_HEAD_KEEP", "1");
    set_env("AGENT_MEMORY_COMPACT_TAIL_KEEP", "8");

    internal::AgentThreadState st = make_linear_history(9);
    const std::size_t n0 = st.history.size();
    MemoryCompactOptions opt;
    const MemoryCompactResult r =
        run_memory_compaction(st, MemoryCompactTrigger::manual_compact, opt);
    assert(!r.did_mutate);
    assert(st.history.size() == n0);
}

void test_c3_manual_bypasses_auto_throttle() {
    fill_compact_mode_truncate();
    set_env("AGENT_MEMORY_COMPACT_HEAD_KEEP", "1");
    set_env("AGENT_MEMORY_COMPACT_TAIL_KEEP", "8");
    set_env("AGENT_MEMORY_AUTO_COMPACT", "1");
    set_env("AGENT_MEMORY_SOFT_LIMIT_BYTES", "80");
    set_env("AGENT_MEMORY_COMPACT_TRIGGER_RATIO", "0.5");
    set_env("AGENT_MEMORY_AUTO_MIN_STEPS", "50");

    internal::AgentThreadState st = make_linear_history(20);
    st.iteration = 3;
    st.last_memory_auto_compact_iteration = 3;

    MemoryCompactOptions mcopt;
    maybe_auto_compact_memory(st, mcopt);
    assert(st.history.size() == 20);

    std::vector<ControlAction> actions;
    ControlAction a;
    a.command = "memory.compact";
    actions.push_back(std::move(a));
    dispatch_pending_control_actions(actions, nullptr, &st, nullptr, nullptr);
    assert(st.history.size() == 10);
}

} // namespace

int main() {
    test_c1_truncate_shape();
    test_c2_noop_small_n();
    test_c3_manual_bypasses_auto_throttle();
    return 0;
}
