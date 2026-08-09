#include <agent/ui/presentation_model.hpp>
#include <agent/ui/streaming_markdown.hpp>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <utility>

namespace agent_framework {

UiPresentationModel::UiPresentationModel(std::size_t max_turns, std::size_t max_tools,
                                         std::size_t max_text_bytes)
    : max_turns_(std::max<std::size_t>(2, max_turns)),
      max_tools_(std::max<std::size_t>(1, max_tools)),
      max_text_bytes_(std::max<std::size_t>(1024, max_text_bytes)) {}

std::int64_t UiPresentationModel::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void UiPresentationModel::append_utf8_capped(std::string& dst, std::string_view text,
                                             std::size_t cap) {
    dst.append(text.data(), text.size());
    if (dst.size() <= cap) return;
    std::size_t cut = dst.size() - cap;
    while (cut < dst.size() && (static_cast<unsigned char>(dst[cut]) & 0xC0U) == 0x80U) ++cut;
    dst.erase(0, cut);
}

void UiPresentationModel::set_runtime_metadata(std::string session_id, std::string provider,
                                               std::string model,
                                               std::string connection_label) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.session_id = std::move(session_id);
    state_.provider = std::move(provider);
    state_.model = std::move(model);
    state_.connection_label = std::move(connection_label);
}

void UiPresentationModel::begin_user_turn(std::string prompt) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.run_state = UiRunState::Running;
    state_.last_error.clear();
    UiTurn user;
    user.role = UiTurnRole::User;
    user.content = prompt;
    user.raw_markdown = std::move(prompt);
    user.timestamp_ms = now_ms();
    state_.turns.push_back(std::move(user));
    UiTurn assistant;
    assistant.role = UiTurnRole::Assistant;
    assistant.timestamp_ms = now_ms();
    assistant.streaming = true;
    state_.turns.push_back(std::move(assistant));
    trim_locked();
}

void UiPresentationModel::ensure_assistant_turn_locked() {
    if (state_.turns.empty() || state_.turns.back().role != UiTurnRole::Assistant ||
        !state_.turns.back().streaming) {
        UiTurn assistant;
        assistant.role = UiTurnRole::Assistant;
        assistant.timestamp_ms = now_ms();
        assistant.streaming = true;
        state_.turns.push_back(std::move(assistant));
    }
}

void UiPresentationModel::append_stream_token(std::string_view token) {
    std::string markdown;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_.run_state == UiRunState::Idle) state_.run_state = UiRunState::Running;
        ensure_assistant_turn_locked();
        append_utf8_capped(state_.turns.back().content, token, max_text_bytes_);
        append_utf8_capped(state_.turns.back().raw_markdown, token, max_text_bytes_);
        markdown = state_.turns.back().raw_markdown;
        trim_locked();
    }
    // Parsing can become moderately expensive for tables/fences. Keep it outside the UI lock.
    auto blocks = StreamingMarkdownAssembler{}.parse(markdown, false);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_.turns.empty() && state_.turns.back().role == UiTurnRole::Assistant &&
        state_.turns.back().raw_markdown == markdown) {
        auto& existing = state_.turns.back().blocks;
        std::copy_if(existing.begin(), existing.end(), std::back_inserter(blocks),
                     [](const UiContentBlock& block) {
                         return block.kind == UiContentBlockKind::Image;
                     });
        existing = std::move(blocks);
    }
}

void UiPresentationModel::append_thinking_token(std::string_view token) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.run_state == UiRunState::Idle) state_.run_state = UiRunState::Running;
    ensure_assistant_turn_locked();
    append_utf8_capped(state_.turns.back().thinking_raw, token, max_text_bytes_);
    trim_locked();
}

bool UiPresentationModel::observe_artifact(const json& payload) {
    if (!payload.is_object() || !payload.contains("id") || !payload["id"].is_string() ||
        !payload.contains("mime") || !payload["mime"].is_string() ||
        !payload.contains("path") || !payload["path"].is_string()) {
        return false;
    }
    const auto& path = payload["path"].get_ref<const std::string&>();
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string::npos) return false;
    std::size_t segment_begin = 0;
    while (segment_begin <= path.size()) {
        const auto segment_end = path.find('/', segment_begin);
        const auto segment = std::string_view(path).substr(
            segment_begin, segment_end == std::string::npos ? std::string::npos
                                                            : segment_end - segment_begin);
        if (segment.empty() || segment == "." || segment == "..") return false;
        if (segment_end == std::string::npos) break;
        segment_begin = segment_end + 1;
    }
    UiAttachment attachment;
    attachment.id = payload["id"].get<std::string>();
    attachment.mime = payload["mime"].get<std::string>();
    attachment.path = path;
    attachment.caption = payload.value("caption", std::string{});
    attachment.tool_call_id = payload.value("tool_call_id", std::string{});
    attachment.sha256 = payload.value("sha256", std::string{});
    attachment.citation_id = payload.value("citation_id", std::string{});
    attachment.source_uri = payload.value("source_uri", std::string{});
    attachment.byte_size = payload.value("byte_size", std::size_t{0});

    std::lock_guard<std::mutex> lock(mutex_);
    ensure_assistant_turn_locked();
    auto& turn = state_.turns.back();
    const auto duplicate = std::find_if(turn.attachments.begin(), turn.attachments.end(),
                                        [&](const UiAttachment& a) { return a.id == attachment.id; });
    if (duplicate != turn.attachments.end()) return false;
    UiContentBlock block;
    block.kind = UiContentBlockKind::Image;
    block.attachment_id = attachment.id;
    block.text = attachment.caption;
    turn.attachments.push_back(std::move(attachment));
    turn.blocks.push_back(std::move(block));
    trim_locked();
    return true;
}

void UiPresentationModel::complete(const json& result) {
    std::string markdown;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_assistant_turn_locked();
        UiTurn& turn = state_.turns.back();
        if (turn.raw_markdown.empty() && result.contains("final_answer") && result["final_answer"].is_string()) {
            const auto& answer = result["final_answer"].get_ref<const std::string&>();
            append_utf8_capped(turn.raw_markdown, answer, max_text_bytes_);
            append_utf8_capped(turn.content, answer, max_text_bytes_);
        }
        // Only explicitly displayable summaries may enter the UI. Raw `reasoning` is internal.
        if (turn.thinking_raw.empty()) {
            const char* keys[] = {"displayable_reasoning", "reasoning_summary"};
            for (const char* key : keys) {
                if (result.contains(key) && result[key].is_string()) {
                    append_utf8_capped(turn.thinking_raw, result[key].get_ref<const std::string&>(),
                                       max_text_bytes_);
                    break;
                }
            }
        }
        turn.streaming = false;
        markdown = turn.raw_markdown;
        state_.run_state = UiRunState::Completed;
        state_.connection_label = "Ready";
        trim_locked();
    }
    auto blocks = StreamingMarkdownAssembler{}.parse(markdown, true);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_.turns.empty() && state_.turns.back().role == UiTurnRole::Assistant &&
        state_.turns.back().raw_markdown == markdown) {
        auto& existing = state_.turns.back().blocks;
        std::copy_if(existing.begin(), existing.end(), std::back_inserter(blocks),
                     [](const UiContentBlock& block) {
                         return block.kind == UiContentBlockKind::Image;
                     });
        existing = std::move(blocks);
    }
}

void UiPresentationModel::fail(std::string message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_.turns.empty() && state_.turns.back().role == UiTurnRole::Assistant &&
        state_.turns.back().streaming) {
        state_.turns.back().streaming = false;
        state_.turns.back().error = true;
    }
    state_.last_error = message;
    UiTurn notice;
    notice.role = UiTurnRole::System;
    notice.content = message;
    notice.raw_markdown = std::move(message);
    notice.timestamp_ms = now_ms();
    notice.error = true;
    state_.turns.push_back(std::move(notice));
    state_.run_state = UiRunState::Failed;
    state_.connection_label = "Error";
    trim_locked();
}

void UiPresentationModel::cancel(std::string message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_.turns.empty() && state_.turns.back().streaming) state_.turns.back().streaming = false;
    UiTurn notice;
    notice.role = UiTurnRole::System;
    notice.content = message;
    notice.raw_markdown = std::move(message);
    notice.timestamp_ms = now_ms();
    state_.turns.push_back(std::move(notice));
    state_.run_state = UiRunState::Cancelled;
    state_.connection_label = "Ready";
    trim_locked();
}

void UiPresentationModel::add_system_notice(std::string message, bool error) {
    std::lock_guard<std::mutex> lock(mutex_);
    UiTurn notice;
    notice.role = UiTurnRole::System;
    notice.content = message;
    notice.raw_markdown = std::move(message);
    notice.timestamp_ms = now_ms();
    notice.error = error;
    state_.turns.push_back(std::move(notice));
    trim_locked();
}

bool UiPresentationModel::result_is_error(const json& result) {
    if (!result.is_object()) return false;
    if (result.contains("error")) return true;
    if (result.contains("ok") && result["ok"].is_boolean()) return !result["ok"].get<bool>();
    return false;
}

void UiPresentationModel::observe_tool(const ToolExecutionEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(state_.tools.rbegin(), state_.tools.rend(), [&](const auto& tool) {
        return !event.tool_call_id.empty() && tool.tool_call_id == event.tool_call_id;
    });
    if (event.phase == ToolExecutionPhase::Started || it == state_.tools.rend()) {
        UiToolActivity activity;
        activity.tool_call_id = event.tool_call_id;
        activity.tool_name = event.tool_name;
        activity.arguments = event.arguments;
        activity.state = UiRunState::Running;
        activity.started_at_ms = now_ms();
        state_.tools.push_back(std::move(activity));
    } else {
        it->result = event.result;
        it->finished_at_ms = now_ms();
        it->duration_ms = std::max<std::int64_t>(0, it->finished_at_ms - it->started_at_ms);
        it->state = result_is_error(event.result) ? UiRunState::Failed : UiRunState::Completed;
    }
    trim_locked();
}

void UiPresentationModel::trim_locked() {
    if (state_.turns.size() > max_turns_) {
        state_.turns.erase(state_.turns.begin(),
                           state_.turns.begin() + static_cast<std::ptrdiff_t>(state_.turns.size() - max_turns_));
    }
    if (state_.tools.size() > max_tools_) {
        state_.tools.erase(state_.tools.begin(),
                           state_.tools.begin() + static_cast<std::ptrdiff_t>(state_.tools.size() - max_tools_));
    }
}

void UiPresentationModel::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto session = state_.session_id;
    const auto provider = state_.provider;
    const auto model = state_.model;
    state_ = {};
    state_.session_id = session;
    state_.provider = provider;
    state_.model = model;
    state_.connection_label = "Ready";
}

void UiPresentationModel::load_demo_state() {
    reset();
    set_runtime_metadata("orbital-analysis", "OpenAI", "deepseek-chat", "MCP connected");
    begin_user_turn("请分析 sin(x) 在 [0, 2π] 上的极值，并给出可复现的计算过程。");
    append_thinking_token("已检查定义域、驻点和端点；下面只展示可验证的推理摘要。");
    append_stream_token(
        "## 计算结果\n\n"
        "在闭区间 $[0, 2\\pi]$ 上，函数的最大值为 **1**，位置为 "
        "$x=\\pi/2\\approx1.5708$。\n\n"
        "| 项目 | 数值 |\n|---|---:|\n| 最大值 | 1.0000 |\n| 横坐标 | 1.5708 rad |\n\n"
        "```python\nimport numpy as np\nx = np.linspace(0, 2*np.pi, 2048)\ny = np.sin(x)\nprint(x[y.argmax()], y.max())\n```\n\n"
        "```mermaid\ngraph LR\n  A[定义域] --> B[求导]\n  B --> C[驻点与端点]\n  C --> D[比较函数值]\n```\n\n"
        "因此，$\\sin(x)$ 在 $x=\\pi/2$ 处取得全局最大值。");
    ToolExecutionEvent a{ToolExecutionPhase::Started, "fs_search", "demo-fs",
                         json{{"query", "*.cpp"}, {"path", "/workspace/src"}}, {}};
    observe_tool(a);
    a.phase = ToolExecutionPhase::Completed;
    a.result = json{{"matches", 18}, {"top", "agent.cpp, tool_mgr.cpp, plot_tool.cpp"}};
    observe_tool(a);
    ToolExecutionEvent b{ToolExecutionPhase::Started, "web_search", "demo-web",
                         json{{"query", "sin(x) maximum 0 to 2pi"}}, {}};
    observe_tool(b);
    b.phase = ToolExecutionPhase::Completed;
    b.result = json{{"sources", 5}, {"status", "verified"}};
    observe_tool(b);
    ToolExecutionEvent c{ToolExecutionPhase::Started, "expr_eval", "demo-expr",
                         json{{"expression", "max(sin(x))"}}, {}};
    observe_tool(c);
    c.phase = ToolExecutionPhase::Completed;
    c.result = json{{"value", 1.0}, {"x", 1.5708}};
    observe_tool(c);
    (void)observe_artifact(json{{"id", "demo-console-reference"},
                                {"mime", "image/png"},
                                {"path", "agent_framework/docs/assets/ui/scientific-console-reference.png"},
                                {"caption", "Scientific console reference artifact"}});
    complete(json{{"final_answer", "demo"}, {"iteration", 3}});
}

UiPresentationSnapshot UiPresentationModel::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

const char* UiPresentationModel::state_name(UiRunState state) noexcept {
    switch (state) {
        case UiRunState::Idle: return "idle";
        case UiRunState::Running: return "running";
        case UiRunState::Completed: return "completed";
        case UiRunState::Failed: return "failed";
        case UiRunState::Cancelled: return "cancelled";
    }
    return "idle";
}

} // namespace agent_framework
