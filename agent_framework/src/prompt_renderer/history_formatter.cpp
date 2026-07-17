/**
 * @file history_formatter.cpp
 * @brief 对话历史格式化器实现
 */

#include <agent/prompt_renderer/prompt_renderer.hpp>

#include <unordered_set>

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

std::vector<std::string> assistant_tool_call_ids(const Message& m) {
    std::vector<std::string> ids;
    if (!is_assistant_tool_calls_message(m)) {
        return ids;
    }
    try {
        const json parsed = json::parse(m.content);
        for (const auto& call : parsed.at("tool_calls")) {
            if (!call.is_object() || !call.contains("id") || !call.at("id").is_string() ||
                call.at("id").get_ref<const std::string&>().empty()) {
                return {};
            }
            ids.push_back(call.at("id").get<std::string>());
        }
    } catch (...) {
        return {};
    }
    return ids;
}

json format_message(const Message& m) {
    json out = json{{"role", m.role}, {"content", m.content}};
    if (m.role == "assistant" && is_assistant_tool_calls_message(m)) {
        const json parsed = json::parse(m.content);
        out["content"] = nullptr;
        out["tool_calls"] = parsed.at("tool_calls");
    } else if (m.role == "tool") {
        if (m.tool_call_id && !m.tool_call_id->empty()) {
            out["tool_call_id"] = *m.tool_call_id;
        } else {
            out["name"] = m.tool_name.value_or("");
        }
        if (m.tool_result) {
            out["content"] = m.tool_result->dump();
        }
    }
    return out;
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
    for (std::size_t i = 0; i < history.size();) {
        const Message& m = history[i];
        if (m.role == "tool") {
            // Never emit an orphan tool message. OpenAI rejects it even when a legacy provider
            // accepted the name-only form.
            ++i;
            continue;
        }
        if (!is_assistant_tool_calls_message(m)) {
            out.push_back(format_message(m));
            ++i;
            continue;
        }

        const std::vector<std::string> expected = assistant_tool_call_ids(m);
        std::unordered_set<std::string> remaining(expected.begin(), expected.end());
        std::vector<json> tool_messages;
        std::size_t j = i + 1;
        while (j < history.size() && history[j].role == "tool") {
            const Message& tool = history[j];
            if (tool.tool_call_id && remaining.erase(*tool.tool_call_id) != 0U) {
                tool_messages.push_back(format_message(tool));
            }
            ++j;
        }
        if (!expected.empty() && remaining.empty() && tool_messages.size() == expected.size()) {
            out.push_back(format_message(m));
            out.insert(out.end(), tool_messages.begin(), tool_messages.end());
        }
        // Drop an incomplete/invalid assistant+tool group as one unit. This also repairs sessions
        // persisted by older versions that capped tool execution after recording all calls.
        i = j;
    }
    return out;
}

std::vector<Message> OpenAIHistoryFormatter::truncate(const std::vector<Message>& history,
                                                      int max_messages) {
    if (max_messages <= 0 || static_cast<int>(history.size()) <= max_messages) {
        return history;
    }
    std::size_t begin = history.size() - static_cast<std::size_t>(max_messages);
    // If the suffix starts inside a tool-result run, discard the remainder of that entire group.
    // Moving forward (rather than backward to its assistant) keeps the configured hard cap.
    while (begin < history.size() && history[begin].role == "tool") {
        ++begin;
    }
    return {history.begin() + static_cast<std::ptrdiff_t>(begin), history.end()};
}

} // namespace agent_framework
