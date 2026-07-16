/**
 * @file working_memory_metrics.cpp
 * @brief WP2.9 working_memory_metrics
 */

#include <agent/context_budget/context_budget.hpp>
#include <agent/internal/agent_thread_state.hpp>
#include <agent/agent/working_memory_metrics.hpp>

#include <cstdlib>
#include <string>

namespace agent_framework {
namespace {

std::size_t memory_soft_limit_from_env() {
    const char* e = std::getenv("AGENT_MEMORY_SOFT_LIMIT_BYTES");
    if (!e || !*e) {
        return 1048576;
    }
    return static_cast<std::size_t>(std::strtoull(e, nullptr, 10));
}

std::size_t memory_hard_limit_from_env() {
    const char* e = std::getenv("AGENT_MEMORY_HARD_LIMIT_BYTES");
    if (!e || !*e) {
        return 2097152;
    }
    return static_cast<std::size_t>(std::strtoull(e, nullptr, 10));
}

double memory_trigger_ratio_from_env() {
    const char* e = std::getenv("AGENT_MEMORY_COMPACT_TRIGGER_RATIO");
    if (!e || !*e) {
        return 0.5;
    }
    const double r = std::strtod(e, nullptr);
    if (r < 0.0) {
        return 0.5;
    }
    if (r > 1.0) {
        return 1.0;
    }
    return r;
}

} // namespace

std::size_t history_message_utf8_bytes(const Message& m) {
    if (m.role == "tool" && m.tool_result) {
        return json_utf8_dump_bytes(*m.tool_result);
    }
    return m.content.size();
}

std::size_t history_utf8_bytes_total(const internal::AgentThreadState& st) {
    std::size_t sum = 0;
    for (const auto& msg : st.history) {
        sum += history_message_utf8_bytes(msg);
    }
    return sum;
}

json working_memory_metrics(const internal::AgentThreadState& st) {
    const std::size_t soft = memory_soft_limit_from_env();
    const std::size_t hard = memory_hard_limit_from_env();
    const double trig = memory_trigger_ratio_from_env();

    std::size_t tool_count = 0;
    std::size_t tool_bytes = 0;
    std::size_t hist_bytes = 0;
    for (const auto& msg : st.history) {
        const std::size_t b = history_message_utf8_bytes(msg);
        hist_bytes += b;
        if (msg.role == "tool") {
            ++tool_count;
            tool_bytes += b;
        }
    }

    json j;
    j["history_message_count"] = st.history.size();
    j["history_utf8_bytes"] = hist_bytes;
    j["tool_messages_count"] = tool_count;
    j["tool_results_utf8_bytes"] = tool_bytes;
    j["soft_limit_bytes"] = soft;
    j["hard_limit_bytes"] = hard;
    j["trigger_ratio"] = trig;
    if (soft == 0) {
        j["effective_usage_ratio"] = 0.0;
        j["would_auto_trigger"] = false;
    } else {
        j["effective_usage_ratio"] =
            static_cast<double>(hist_bytes) / static_cast<double>(soft);
        const double threshold = static_cast<double>(soft) * trig;
        j["would_auto_trigger"] = hist_bytes >= threshold;
    }
    return j;
}

} // namespace agent_framework
