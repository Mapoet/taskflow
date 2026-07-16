/**
 * @file tool_orchestration.cpp
 * @brief WP2.1b tool orchestration implementation
 */

#include <agent/toolbus/toolbus.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <future>
#include <string>

namespace agent_framework {
namespace {

void notify_tool_observer(const ToolExecutionObserver& observer,
                          ToolExecutionEvent event) noexcept {
    if (!observer) return;
    try {
        observer(event);
    } catch (...) {
        // Observability must not alter tool execution semantics.
    }
}

constexpr const char* kA2aSubmitTaskName = "a2a_submit_task";

void ascii_lower_inplace(std::string& s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
}

bool env_truthy(const char* raw) {
    std::string s(raw);
    ascii_lower_inplace(s);
    return s == "1" || s == "true" || s == "on" || s == "yes";
}

bool env_falsy(const char* raw) {
    std::string s(raw);
    ascii_lower_inplace(s);
    return s == "0" || s == "false" || s == "off" || s == "no";
}

bool is_readonly_for_grouping(ToolSideEffect se) {
    return se == ToolSideEffect::ReadOnly;
}

} // namespace

ToolOrchestrationOptions resolve_tool_orchestration_options(const AgentConfig& cfg) {
    ToolOrchestrationOptions o;
    o.enable_parallel_reads = cfg.enable_parallel_read_tools;
    int maxp = cfg.max_parallel_read_tools;
    if (maxp <= 0) {
        maxp = 1;
    }
    o.max_parallel_reads = maxp;

    o.enable_parallel_a2a_submits = cfg.enable_parallel_a2a_submits;
    int maxa = cfg.max_parallel_a2a_submits;
    if (maxa <= 0) {
        maxa = 1;
    }
    o.max_parallel_a2a_submits = maxa;

    if (const char* e = std::getenv("AGENT_A2A_MAX_PARALLEL_SUBMITS")) {
        if (e[0] != '\0') {
            try {
                const int v = std::stoi(std::string(e));
                if (v >= 1) {
                    o.max_parallel_a2a_submits = v;
                }
            } catch (...) {
            }
        }
    }

    if (const char* e = std::getenv("AGENT_TOOL_PARALLEL_READS")) {
        if (e[0] != '\0') {
            if (env_falsy(e)) {
                o.enable_parallel_reads = false;
            } else if (env_truthy(e)) {
                o.enable_parallel_reads = true;
            }
        }
    }

    if (const char* e = std::getenv("AGENT_TOOL_MAX_PARALLEL")) {
        if (e[0] != '\0') {
            try {
                const int v = std::stoi(std::string(e));
                if (v >= 1) {
                    o.max_parallel_reads = v;
                }
            } catch (...) {
                // ignore invalid
            }
        }
    }

    if (o.max_parallel_reads <= 0) {
        o.max_parallel_reads = 1;
    }
    if (o.max_parallel_a2a_submits <= 0) {
        o.max_parallel_a2a_submits = 1;
    }
    return o;
}

std::vector<json> execute_tool_calls_sequenced(std::shared_ptr<ToolBus> bus,
                                               const std::vector<CallSpec>& calls,
                                               const ToolOrchestrationOptions& opts,
                                               ToolSideEffectResolver classify,
                                               ToolExecutionObserver observer,
                                               ToolCallControl control) {
    const std::size_t n = calls.size();
    std::vector<json> results(n);

    if (!bus || n == 0U) {
        return results;
    }

    const bool parallel_on = opts.enable_parallel_reads;
    const int max_p = std::max(1, opts.max_parallel_reads);
    const bool a2a_parallel_on = opts.enable_parallel_a2a_submits;
    const int max_a2a = std::max(1, opts.max_parallel_a2a_submits);

    std::size_t i = 0;
    while (i < n) {
        if (control.should_stop()) break;
        const ToolSideEffect se = classify(calls[i].name);

        if (a2a_parallel_on && calls[i].name == kA2aSubmitTaskName) {
            std::size_t j = i + 1;
            while (j < n && calls[j].name == kA2aSubmitTaskName) {
                ++j;
            }
            const std::size_t glen = j - i;
            const std::size_t chunk = static_cast<std::size_t>(max_a2a);
            for (std::size_t chunk_start = 0; chunk_start < glen; chunk_start += chunk) {
                const std::size_t chunk_end = std::min(chunk_start + chunk, glen);
                std::vector<std::future<json>> futs;
                futs.reserve(chunk_end - chunk_start);
                for (std::size_t t = chunk_start; t < chunk_end; ++t) {
                    if (control.should_stop()) break;
                    const std::size_t gi = i + t;
                    notify_tool_observer(observer, {ToolExecutionPhase::Started, calls[gi].name,
                                                    calls[gi].tool_call_id.value_or(""), calls[gi].arguments, {}});
                    futs.push_back(bus->call_tool(calls[gi].name, calls[gi].arguments, control));
                }
                for (std::size_t u = 0; u < futs.size(); ++u) {
                    const std::size_t gi = i + chunk_start + u;
                    results[gi] = futs[u].get();
                    notify_tool_observer(observer, {ToolExecutionPhase::Completed, calls[gi].name,
                                                    calls[gi].tool_call_id.value_or(""), calls[gi].arguments, results[gi]});
                }
            }
            i = j;
            continue;
        }

        if (!parallel_on || !is_readonly_for_grouping(se)) {
            notify_tool_observer(observer, {ToolExecutionPhase::Started, calls[i].name,
                                            calls[i].tool_call_id.value_or(""), calls[i].arguments, {}});
            results[i] = bus->call_tool(calls[i].name, calls[i].arguments, control).get();
            notify_tool_observer(observer, {ToolExecutionPhase::Completed, calls[i].name,
                                            calls[i].tool_call_id.value_or(""), calls[i].arguments, results[i]});
            ++i;
            continue;
        }

        std::size_t j = i + 1;
        while (j < n && is_readonly_for_grouping(classify(calls[j].name))) {
            ++j;
        }

        const std::size_t glen = j - i;
        const std::size_t chunk = static_cast<std::size_t>(max_p);

        for (std::size_t chunk_start = 0; chunk_start < glen; chunk_start += chunk) {
            const std::size_t chunk_end = std::min(chunk_start + chunk, glen);
            std::vector<std::future<json>> futs;
            futs.reserve(chunk_end - chunk_start);
            for (std::size_t t = chunk_start; t < chunk_end; ++t) {
                if (control.should_stop()) break;
                const std::size_t gi = i + t;
                notify_tool_observer(observer, {ToolExecutionPhase::Started, calls[gi].name,
                                                calls[gi].tool_call_id.value_or(""), calls[gi].arguments, {}});
                futs.push_back(bus->call_tool(calls[gi].name, calls[gi].arguments, control));
            }
            for (std::size_t u = 0; u < futs.size(); ++u) {
                const std::size_t gi = i + chunk_start + u;
                results[gi] = futs[u].get();
                notify_tool_observer(observer, {ToolExecutionPhase::Completed, calls[gi].name,
                                                calls[gi].tool_call_id.value_or(""), calls[gi].arguments, results[gi]});
            }
        }

        i = j;
    }

    return results;
}

} // namespace agent_framework
