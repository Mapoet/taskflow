/**
 * @file gui_handler.cpp
 * @brief ImGui output handler — WP2.U（队列投递 + aux 事件）
 */

#include <agent/ui_manager.hpp>

#include <cstdlib>
#include <ctime>
#include <sstream>
#include <utility>

namespace agent_framework {

namespace {

std::size_t imgui_queue_drain_max_from_env() {
    const char* e = std::getenv("AGENT_IMGUI_QUEUE_DRAIN_MAX");
    if (!e || !*e) {
        return 256;
    }
    const auto v = static_cast<std::size_t>(std::strtoull(e, nullptr, 10));
    return v ? v : 256;
}

} // namespace

ImGuiHandler::ImGuiHandler(std::shared_ptr<ThreadSafeQueue<StreamMessage>> queue, std::string session_id)
    : queue_(std::move(queue)), session_id_(std::move(session_id)) {}

void ImGuiHandler::push_message(const std::string& type, const std::string& content) {
    if (!queue_ || !active_) {
        return;
    }
    StreamMessage m;
    m.session_id = session_id_;
    m.message_type = type;
    m.content = content;
    m.timestamp = std::time(nullptr);
    queue_->push(std::move(m));
}

void ImGuiHandler::handle_stream_token(std::string_view token) {
    if (!active_) {
        return;
    }
    push_message("token", std::string(token));
}

void ImGuiHandler::handle_final_result(const json& result) {
    if (!active_) {
        return;
    }
    // WP1.6 / phase-1-wp6 §4.3：与 CLI 一致，终稿不重复 dump已流式展示的全文
    if (result.contains("final_answer") && result["final_answer"].is_string()) {
        const auto& fa = result["final_answer"].get_ref<const std::string&>();
        std::ostringstream oss;
        oss << "[result] iteration=";
        if (result.contains("iteration")) {
            oss << result["iteration"].dump();
        } else {
            oss << "?";
        }
        oss << " history_size=";
        if (result.contains("history_size")) {
            oss << result["history_size"].dump();
        } else {
            oss << "?";
        }
        oss << " final_answer_chars=" << fa.size();
        oss << " (stream showed body; full JSON optional)";
        push_message("final", oss.str());
    } else {
        push_message("final", result.dump());
    }
}

void ImGuiHandler::handle_error(const std::string& error_message) {
    if (!active_) {
        return;
    }
    push_message("error", error_message);
}

void ImGuiHandler::handle_aux_event(std::string_view type, const json& payload) {
    if (!active_) {
        return;
    }
    std::string mt = std::string("aux:") + std::string(type);
    push_message(mt, payload.dump());
}

std::string ImGuiHandler::get_handler_type() const {
    return "imgui";
}

bool ImGuiHandler::is_active() const {
    return active_;
}

std::size_t ImGuiHandler::drain_messages(std::vector<StreamMessage>& out, std::size_t max_n) {
    out.clear();
    if (!queue_) {
        return 0;
    }
    const std::size_t cap = max_n ? max_n : imgui_queue_drain_max_from_env();
    std::size_t n = 0;
    StreamMessage m;
    while (n < cap && queue_->try_pop(m)) {
        out.push_back(std::move(m));
        ++n;
    }
    return n;
}

} // namespace agent_framework
