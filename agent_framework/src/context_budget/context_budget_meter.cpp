/**
 * @file context_budget_meter.cpp
 * @brief 合并帽与注入帽辅助
 */

#include "agent/context_budget.hpp"

#include "agent/types.hpp"

namespace agent_framework {

namespace {

using json = nlohmann::json;

std::size_t tool_results_dump_sum(const std::vector<Message>& history) {
    std::size_t s = 0;
    for (const auto& m : history) {
        if (m.role == "tool" && m.tool_result) {
            s += json_utf8_dump_bytes(*m.tool_result);
        }
    }
    return s;
}

} // namespace

std::size_t ContextBudgetMeter::combined_attach_bytes(const std::vector<Message>& history,
                                                      const std::string& user_prompt_utf8,
                                                      const std::string& context_utf8,
                                                      const std::string& skill_block_utf8) {
    return tool_results_dump_sum(history) + user_prompt_utf8.size() + context_utf8.size() +
           skill_block_utf8.size();
}

void ContextBudgetMeter::apply_combined_to_history_slice(std::vector<Message>& history_for_render,
                                                         const std::string& user_prompt_utf8,
                                                         const std::string& context_utf8,
                                                         const std::string& skill_block_utf8,
                                                         const ContextBudgetLimits& limits) {
    const std::size_t max_c = limits.max_combined_prompt_attach_bytes;
    for (;;) {
        const std::size_t text_sum =
            user_prompt_utf8.size() + context_utf8.size() + skill_block_utf8.size();
        const std::size_t ts = tool_results_dump_sum(history_for_render);
        if (ts + text_sum <= max_c) {
            return;
        }
        bool progressed = false;
        for (auto& m : history_for_render) {
            if (m.role != "tool" || !m.tool_result) {
                continue;
            }
            if (is_combined_budget_stub(*m.tool_result)) {
                continue;
            }
            const std::size_t orig = json_utf8_dump_bytes(*m.tool_result);
            m.tool_result = combined_budget_exhausted_placeholder(orig);
            progressed = true;
            break;
        }
        if (!progressed) {
            return;
        }
    }
}

void apply_injection_cap(std::string& context_utf8, std::string& skill_block_utf8,
                         std::string& user_prompt_utf8, const ContextBudgetLimits& limits,
                         std::string& out_af_budget_section) {
    out_af_budget_section.clear();
    const std::size_t max_b = limits.max_injection_bytes;
    const std::size_t total =
        context_utf8.size() + skill_block_utf8.size() + user_prompt_utf8.size();
    if (total <= max_b) {
        return;
    }
    static constexpr const char kDelim[] = "\n<<AF_INJ>>\n";
    std::string merged = context_utf8 + kDelim + skill_block_utf8 + kDelim + user_prompt_utf8;
    const std::size_t reserve_meta = 320;
    const std::size_t keep = max_b > reserve_meta ? max_b - reserve_meta : 0;
    merged = utf8_safe_truncate(merged, keep);
    std::vector<std::string> parts;
    std::size_t pos = 0;
    const std::size_t dlen = sizeof(kDelim) - 1;
    while (pos <= merged.size()) {
        const std::size_t f = merged.find(kDelim, pos);
        if (f == std::string::npos) {
            parts.push_back(merged.substr(pos));
            break;
        }
        parts.push_back(merged.substr(pos, f - pos));
        pos = f + dlen;
    }
    context_utf8 = parts.size() > 0 ? std::move(parts[0]) : std::string();
    skill_block_utf8 = parts.size() > 1 ? std::move(parts[1]) : std::string();
    user_prompt_utf8 = parts.size() > 2 ? std::move(parts[2]) : std::string();
    AfTruncationMeta meta;
    meta.kind = AfTruncationKind::user_injection;
    meta.original_utf8_bytes = total;
    meta.kept_utf8_bytes = merged.size();
    meta.spill = false;
    meta.reason = "injection_cap";
    json j = json::object();
    j["_af_truncation"] = json{{"kind", af_truncation_kind_string(meta.kind)},
                               {"original_utf8_bytes", meta.original_utf8_bytes},
                               {"kept_utf8_bytes", meta.kept_utf8_bytes},
                               {"spill", false},
                               {"ref", ""},
                               {"reason", meta.reason}};
    out_af_budget_section = std::string("\n## af_budget\n") + j.dump();
}

} // namespace agent_framework
