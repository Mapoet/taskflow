#include "agent/telemetry/trace_context.hpp"

#include <algorithm>
#include <cctype>

namespace agent_framework::telemetry {
namespace {
bool hex(std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isxdigit(c) && !std::isupper(c);
    });
}
bool zero(std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](char c) { return c == '0'; });
}
}

std::optional<W3CTraceContext> parse_traceparent(std::string_view value,
                                                 std::string_view trace_state) {
    if (value.size() != 55 || value[2] != '-' || value[35] != '-' || value[52] != '-')
        return std::nullopt;
    const auto version = value.substr(0, 2);
    const auto trace = value.substr(3, 32);
    const auto parent = value.substr(36, 16);
    const auto flags = value.substr(53, 2);
    if (!hex(version) || version == "ff" || !hex(trace) || zero(trace) ||
        !hex(parent) || zero(parent) || !hex(flags)) return std::nullopt;
    unsigned flag_value = 0;
    for (const char c : flags) flag_value = flag_value * 16U +
        static_cast<unsigned>(c <= '9' ? c - '0' : c - 'a' + 10);
    W3CTraceContext out{std::string(trace), std::string(parent),
                        (flag_value & 1U) != 0, std::string(trace_state)};
    if (out.trace_state.find('\n') != std::string::npos ||
        out.trace_state.find('\r') != std::string::npos || out.trace_state.size() > 512)
        return std::nullopt;
    return out;
}

std::string format_traceparent(const W3CTraceContext& context) {
    if (context.trace_id.size() != 32 || context.parent_id.size() != 16 ||
        !hex(context.trace_id) || zero(context.trace_id) ||
        !hex(context.parent_id) || zero(context.parent_id)) return {};
    return "00-" + context.trace_id + "-" + context.parent_id +
           (context.sampled ? "-01" : "-00");
}
}  // namespace agent_framework::telemetry
