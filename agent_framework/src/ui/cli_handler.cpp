/**
 * @file cli_handler.cpp
 * @brief CLI output handler (WP1.6)
 */

#include <agent/ui/ui_manager.hpp>
#include <agent/ui/phase4_operations.hpp>

#include <iostream>
#include <sstream>

namespace agent_framework {

CLIHandler::CLIHandler(std::ostream& output_stream) : output_stream_(output_stream) {}

void CLIHandler::handle_stream_token(std::string_view token) {
    if (!active_) {
        return;
    }
    std::lock_guard<std::mutex> lock(output_mutex_);
    output_stream_ << token;
    output_stream_.flush();
}

void CLIHandler::handle_final_result(const json& result) {
    if (!active_) {
        return;
    }
    std::lock_guard<std::mutex> lock(output_mutex_);
    // WP1.6 §4.3: when stream_callback already printed assistant text, avoid dumping the full
    // final_answer again. Emit a short summary; callers may use non-streaming mode for full JSON.
    if (result.contains("final_answer") && result["final_answer"].is_string()) {
        const auto& fa = result["final_answer"].get_ref<const std::string&>();
        output_stream_ << "\n[result] iteration=";
        if (result.contains("iteration")) {
            output_stream_ << result["iteration"].dump();
        } else {
            output_stream_ << "?";
        }
        output_stream_ << " history_size=";
        if (result.contains("history_size")) {
            output_stream_ << result["history_size"].dump();
        } else {
            output_stream_ << "?";
        }
        output_stream_ << " final_answer_chars=" << fa.size();
        output_stream_ << " (stream may have printed body; use non-streaming to print full JSON)\n";
    } else {
        std::ostringstream oss;
        oss << result.dump(2);
        format_output(oss.str(), "[result]");
    }
    output_stream_.flush();
}

void CLIHandler::handle_error(const std::string& error_message) {
    if (!active_) {
        return;
    }
    std::cerr << "[error] " << error_message << std::endl;
}

void CLIHandler::handle_aux_event(std::string_view type, const json& payload) {
    if (!active_ || type != Phase4OperationsProjection::event_type) return;
    const auto snapshot = Phase4OperationsProjection::from_json(payload);
    std::lock_guard<std::mutex> lock(output_mutex_);
    output_stream_ << Phase4OperationsProjection::render_text(snapshot);
    output_stream_.flush();
}

std::string CLIHandler::get_handler_type() const {
    return "cli";
}

bool CLIHandler::is_active() const {
    return active_;
}

void CLIHandler::format_output(const std::string& content, const std::string& prefix) {
    if (prefix.empty()) {
        output_stream_ << content;
    } else {
        output_stream_ << prefix << " " << content << "\n";
    }
}

} // namespace agent_framework
