#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace agent_framework::telemetry {

struct W3CTraceContext {
    std::string trace_id;
    std::string parent_id;
    bool sampled{false};
    std::string trace_state;
};

std::optional<W3CTraceContext> parse_traceparent(std::string_view value,
                                                 std::string_view trace_state = {});
std::string format_traceparent(const W3CTraceContext& context);

}  // namespace agent_framework::telemetry
