/**
 * @file web_handler.cpp
 * @brief Web output handler — stage 1 stub (WP1.6 §1.3)
 */

#include <agent/ui_manager.hpp>

namespace agent_framework {

WebHandler::WebHandler(const std::string& session_id,
                       std::shared_ptr<WebConnectionInfo> connection)
    : session_id_(session_id), connection_(std::move(connection)) {}

void WebHandler::handle_stream_token(std::string_view /*token*/) {}

void WebHandler::handle_final_result(const json& /*result*/) {}

void WebHandler::handle_error(const std::string& /*error_message*/) {}

std::string WebHandler::get_handler_type() const {
    return "web";
}

bool WebHandler::is_active() const {
    return active_;
}

void WebHandler::send_sse_event(const std::string& /*event_type*/, const std::string& /*data*/) {}

void WebHandler::send_ws_message(const json& /*message*/) {}

bool WebHandler::check_connection() const {
    return connection_ && connection_->is_active;
}

} // namespace agent_framework
