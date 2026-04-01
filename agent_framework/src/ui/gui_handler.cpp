/**
 * @file gui_handler.cpp
 * @brief ImGui output handler — stage 1 stub (WP1.6 §1.3)
 */

#include <agent/ui_manager.hpp>

namespace agent_framework {

ImGuiHandler::ImGuiHandler(std::shared_ptr<ThreadSafeQueue<StreamMessage>> queue)
    : queue_(std::move(queue)) {}

void ImGuiHandler::handle_stream_token(std::string_view /*token*/) {}

void ImGuiHandler::handle_final_result(const json& /*result*/) {}

void ImGuiHandler::handle_error(const std::string& /*error_message*/) {}

std::string ImGuiHandler::get_handler_type() const {
    return "imgui";
}

bool ImGuiHandler::is_active() const {
    return active_;
}

void ImGuiHandler::push_message(const std::string& /*type*/, const std::string& /*content*/) {}

} // namespace agent_framework
