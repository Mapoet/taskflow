/**
 * @file tui_handler.cpp
 * @brief TuiHandler 实现
 */

#include <agent/ui/tui_handler.hpp>

#include <sstream>

namespace agent_framework {

namespace {

void utf8_erase_prefix_if_over(std::string& s, std::size_t max_bytes) {
    while (s.size() > max_bytes) {
        std::size_t cut = s.size() - max_bytes;
        while (cut < s.size() && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) {
            ++cut;
        }
        if (cut >= s.size()) {
            break;
        }
        s.erase(0, cut);
    }
}

} // namespace

TuiHandler::TuiHandler(std::shared_ptr<UiPresentationModel> presentation)
    : presentation_(std::move(presentation)) {}

void TuiHandler::append_capped(std::string& buf, std::string_view chunk, std::size_t max_bytes) {
    buf.append(chunk.data(), chunk.size());
    utf8_erase_prefix_if_over(buf, max_bytes);
}

void TuiHandler::handle_stream_token(std::string_view token) {
    if (!active_) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    append_capped(stream_text_, token, k_max_stream_bytes);
    if (presentation_) presentation_->append_stream_token(token);
}

void TuiHandler::handle_final_result(const json& result) {
    if (!active_) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (presentation_) presentation_->complete(result);
    if (result.contains("final_answer") && result["final_answer"].is_string()) {
        const auto& fa = result["final_answer"].get_ref<const std::string&>();
        std::ostringstream oss;
        oss << "\n[result] iteration=";
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
        oss << " (stream showed body; full JSON optional)\n";
        append_capped(stream_text_, oss.str(), k_max_stream_bytes);
    } else {
        append_capped(stream_text_, result.dump(), k_max_stream_bytes);
    }
}

void TuiHandler::handle_error(const std::string& error_message) {
    if (!active_) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (presentation_) presentation_->fail(error_message);
    append_capped(stream_text_, std::string("\n[error] ") + error_message + "\n", k_max_stream_bytes);
}

void TuiHandler::handle_aux_event(std::string_view type, const json& payload) {
    if (!active_) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    std::string line = std::string("aux:") + std::string(type) + " " + payload.dump() + "\n";
    append_capped(aux_text_, line, k_max_aux_bytes);
}

std::string TuiHandler::get_handler_type() const {
    return "tui";
}

bool TuiHandler::is_active() const {
    return active_;
}

TuiHandler::DisplaySnapshot TuiHandler::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return DisplaySnapshot{stream_text_, aux_text_};
}

UiPresentationSnapshot TuiHandler::presentation_snapshot() const {
    if (presentation_) return presentation_->snapshot();
    return {};
}

} // namespace agent_framework
