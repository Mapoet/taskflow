/**
 * @file web_handler.cpp
 * @brief Web output handler — WP2.U（SSE 块队列 + JSON 行）
 */

#include <agent/ui_manager.hpp>

#include <utility>

namespace agent_framework {

WebHandler::WebHandler(const std::string& session_id, std::shared_ptr<WebConnectionInfo> connection)
    : session_id_(session_id), connection_(std::move(connection)) {}

void WebHandler::handle_stream_token(std::string_view token) {
    if (!active_) {
        return;
    }
    json j;
    j["kind"] = "token";
    j["session"] = session_id_;
    j["content"] = std::string(token);
    send_sse_event("token", j.dump());
}

void WebHandler::handle_final_result(const json& result) {
    if (!active_) {
        return;
    }
    json j;
    j["kind"] = "final";
    j["session"] = session_id_;
    if (result.contains("final_answer")) {
        j["final_answer"] = result["final_answer"];
    }
    if (result.contains("iteration")) {
        j["iteration"] = result["iteration"];
    }
    if (result.contains("history_size")) {
        j["history_size"] = result["history_size"];
    }
    send_sse_event("final", j.dump());
}

void WebHandler::handle_error(const std::string& error_message) {
    if (!active_) {
        return;
    }
    json j;
    j["kind"] = "error";
    j["session"] = session_id_;
    j["message"] = error_message;
    send_sse_event("error", j.dump());
}

void WebHandler::handle_aux_event(std::string_view type, const json& payload) {
    if (!active_) {
        return;
    }
    json j;
    j["kind"] = "aux";
    j["session"] = session_id_;
    j["type"] = std::string(type);
    j["payload"] = payload;
    send_sse_event("aux", j.dump());
}

std::string WebHandler::get_handler_type() const {
    return "web";
}

bool WebHandler::is_active() const {
    return active_;
}

void WebHandler::send_sse_event(const std::string& /*event_type*/, const std::string& json_payload) {
    std::lock_guard<std::mutex> lock(sse_mutex_);
    sse_chunks_.push_back(std::string("data: ") + json_payload + "\n\n");
}

bool WebHandler::try_pop_sse_chunk(std::string& out) {
    std::lock_guard<std::mutex> lock(sse_mutex_);
    if (sse_chunks_.empty()) {
        return false;
    }
    out = std::move(sse_chunks_.front());
    sse_chunks_.pop_front();
    return true;
}

void WebHandler::send_ws_message(const json& /*message*/) {}

bool WebHandler::check_connection() const {
    return connection_ && connection_->is_active;
}

} // namespace agent_framework
