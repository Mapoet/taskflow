/**
 * @file ui_manager.cpp
 * @brief UIManager — minimal WP1.6 implementation
 */

#include <agent/ui/ui_manager.hpp>

#include <utility>

namespace agent_framework {

void UIManager::register_handler(std::unique_ptr<UIHandler> handler) {
    if (!handler) {
        return;
    }
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    handlers_.push_back(std::move(handler));
}

void UIManager::register_cli_handler(std::unique_ptr<CLIHandler> handler) {
    if (!handler) {
        return;
    }
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    handlers_.push_back(std::unique_ptr<UIHandler>(handler.release()));
}

void UIManager::register_gui_handler(std::unique_ptr<ImGuiHandler> handler) {
    if (!handler) {
        return;
    }
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    handlers_.push_back(std::unique_ptr<UIHandler>(handler.release()));
}

void UIManager::register_web_connection(const std::string& session_id,
                                        std::unique_ptr<WebHandler> handler) {
    if (!handler) {
        return;
    }
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    session_handlers_[session_id] = std::unique_ptr<UIHandler>(handler.release());
}

void UIManager::dispatch_message(const std::string& type, const json& data) {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    for (auto& h : handlers_) {
        if (h && h->is_active()) {
            h->handle_aux_event(type, data);
        }
    }
    for (auto& kv : session_handlers_) {
        if (kv.second && kv.second->is_active()) {
            kv.second->handle_aux_event(type, data);
        }
    }
}

void UIManager::dispatch_final_result(const json& result) {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    for (auto& h : handlers_) {
        if (h && h->is_active()) {
            h->handle_final_result(result);
        }
    }
    for (auto& kv : session_handlers_) {
        if (kv.second && kv.second->is_active()) {
            kv.second->handle_final_result(result);
        }
    }
}

void UIManager::dispatch_error(const std::string& error_message) {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    for (auto& h : handlers_) {
        if (h && h->is_active()) {
            h->handle_error(error_message);
        }
    }
    for (auto& kv : session_handlers_) {
        if (kv.second && kv.second->is_active()) {
            kv.second->handle_error(error_message);
        }
    }
}

void UIManager::stream_token(const std::string& /*session_id*/, std::string_view token) {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    for (auto& h : handlers_) {
        if (h && h->is_active()) {
            h->handle_stream_token(token);
        }
    }
    for (auto& kv : session_handlers_) {
        if (kv.second && kv.second->is_active()) {
            kv.second->handle_stream_token(token);
        }
    }
}

void UIManager::unregister_handler(const std::string& handler_id) {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    session_handlers_.erase(handler_id);
}

std::vector<std::string> UIManager::list_active_handlers() const {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    std::vector<std::string> out;
    out.reserve(handlers_.size() + session_handlers_.size());
    for (std::size_t i = 0; i < handlers_.size(); ++i) {
        if (handlers_[i] && handlers_[i]->is_active()) {
            out.push_back("handler_" + std::to_string(i) + ":" + handlers_[i]->get_handler_type());
        }
    }
    for (const auto& kv : session_handlers_) {
        if (kv.second && kv.second->is_active()) {
            out.push_back(kv.first + ":" + kv.second->get_handler_type());
        }
    }
    return out;
}

void UIManager::dispatch_to_all(const std::function<void(UIHandler&)>& action) {
    for (auto& h : handlers_) {
        if (h) {
            action(*h);
        }
    }
    for (auto& kv : session_handlers_) {
        if (kv.second) {
            action(*kv.second);
        }
    }
}

} // namespace agent_framework
