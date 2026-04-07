/**
 * @file test_working_memory_metrics.cpp
 * @brief WP2.9 M-1, M-2
 */

#include <agent/internal/agent_thread_state.hpp>
#include <agent/working_memory_metrics.hpp>

#include <cassert>
#include <cstdlib>
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

void test_m1_empty_history() {
    unset_env("AGENT_MEMORY_SOFT_LIMIT_BYTES");
    unset_env("AGENT_MEMORY_HARD_LIMIT_BYTES");
    unset_env("AGENT_MEMORY_COMPACT_TRIGGER_RATIO");

    internal::AgentThreadState st;
    const json j = working_memory_metrics(st);
    assert(j["history_utf8_bytes"].get<std::size_t>() == 0);
    assert(j["would_auto_trigger"].get<bool>() == false);
}

void test_m2_known_byte_sum() {
    set_env("AGENT_MEMORY_SOFT_LIMIT_BYTES", "10000");
    set_env("AGENT_MEMORY_COMPACT_TRIGGER_RATIO", "0.5");

    internal::AgentThreadState st;
    Message u;
    u.role = "user";
    u.content = "hello";
    st.history.push_back(u);

    Message t;
    t.role = "tool";
    t.tool_result = json{{"x", std::string(100, 'z')}};
    st.history.push_back(t);

    const std::size_t expect =
        history_message_utf8_bytes(st.history[0]) + history_message_utf8_bytes(st.history[1]);
    assert(history_utf8_bytes_total(st) == expect);

    const json j = working_memory_metrics(st);
    assert(j["history_utf8_bytes"].get<std::size_t>() == expect);
    assert(j["tool_messages_count"].get<std::size_t>() == 1);
    assert(j["tool_results_utf8_bytes"].get<std::size_t>() ==
           history_message_utf8_bytes(st.history[1]));
}

void test_d10_metric_keys() {
    internal::AgentThreadState st;
    const json j = working_memory_metrics(st);
    assert(j.contains("history_message_count"));
    assert(j.contains("history_utf8_bytes"));
    assert(j.contains("tool_messages_count"));
    assert(j.contains("tool_results_utf8_bytes"));
    assert(j.contains("soft_limit_bytes"));
    assert(j.contains("hard_limit_bytes"));
    assert(j.contains("trigger_ratio"));
    assert(j.contains("effective_usage_ratio"));
    assert(j.contains("would_auto_trigger"));
}

} // namespace

int main() {
    test_m1_empty_history();
    test_m2_known_byte_sum();
    test_d10_metric_keys();
    return 0;
}
