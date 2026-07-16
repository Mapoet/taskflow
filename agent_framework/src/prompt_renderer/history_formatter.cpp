/**
 * @file history_formatter.cpp
 * @brief 对话历史格式化器实现
 */

#include <agent/prompt_renderer/prompt_renderer.hpp>

namespace agent_framework {

namespace {

bool is_assistant_tool_calls_message(const Message& m) {
    if (m.role != "assistant") {
        return false;
    }
    try {
        json j = json::parse(m.content);
        return j.is_object() && j.contains("tool_calls") && j["tool_calls"].is_array();
    } catch (...) {
        return false;
    }
}

} // namespace

std::string OpenAIHistoryFormatter::format_as_text(const std::vector<Message>& history) {
    std::ostringstream o;
    for (const auto& m : history) {
        o << m.role << ": " << m.content << '\n';
    }
    return o.str();
}

std::vector<json> OpenAIHistoryFormatter::format_as_messages(const std::vector<Message>& history) {
    std::vector<json> out;
    out.reserve(history.size());
    for (const auto& m : history) {
        json j = json{{"role", m.role}, {"content", m.content}};
        if (m.role == "assistant" && is_assistant_tool_calls_message(m)) {
            try {
                json parsed = json::parse(m.content);
                j["content"] = nullptr;
                j["tool_calls"] = parsed["tool_calls"];
            } catch (...) {
                // fallback to plain content
            }
        }
        if (m.role == "tool") {
            if (m.tool_call_id && !m.tool_call_id->empty()) {
                j["tool_call_id"] = *m.tool_call_id;
            } else {
                // Backward-compat: some providers accept "name" for tool role.
                j["name"] = m.tool_name.value_or("");
            }
            if (m.tool_result) {
                j["content"] = m.tool_result->dump();
            }
        }
        out.push_back(std::move(j));
    }
    return out;
}

std::vector<Message> OpenAIHistoryFormatter::truncate(const std::vector<Message>& history,
                                                      int max_messages) {
    if (max_messages <= 0 || static_cast<int>(history.size()) <= max_messages) {
        return history;
    }
    std::vector<Message> out(history.end() - max_messages, history.end());

    // 成对约束（启发式）：避免开头落在 tool 组中间（tool 或 assistant(tool_calls) 之后的残片）
    // 规则：如果开头不是 user 且属于 tool 相关，则持续删除直到遇到 user 或清空。
    while (!out.empty()) {
        const Message& first = out.front();
        const bool tool_related =
            (first.role == "tool") || is_assistant_tool_calls_message(first);
        if (!tool_related) {
            break;
        }
        // 进一步收敛到 user 边界
        out.erase(out.begin());
    }
    while (!out.empty() && out.front().role != "user" && out.front().role != "assistant" &&
           out.front().role != "system") {
        out.erase(out.begin());
    }
    // 若仍以 tool 开头（例如连续 tool），继续剔除
    while (!out.empty() && out.front().role == "tool") {
        out.erase(out.begin());
    }
    return out;
}

} // namespace agent_framework

