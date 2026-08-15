/**
 * @file web_handler.cpp
 * @brief Web output handler — WP2.U（SSE 块队列 + JSON 行）
 */

#include <agent/ui/ui_manager.hpp>

#include <algorithm>
#include <utility>

namespace agent_framework {

WebHandler::WebHandler(const std::string& session_id, std::shared_ptr<WebConnectionInfo> connection)
    : session_id_(session_id), connection_(std::move(connection)) {}

void WebHandler::handle_stream_token(std::string_view token) {
    handle_stream_chunk(UiStreamChannel::Answer, token);
}

void WebHandler::handle_stream_chunk(UiStreamChannel channel, std::string_view token) {
    if (!active_) {
        return;
    }
    json j;
    const bool thinking = channel == UiStreamChannel::Thinking;
    j["kind"] = thinking ? "thinking" : "token";
    j["session"] = session_id_;
    j["content"] = std::string(token);
    send_sse_event(thinking ? "thinking" : "token", j.dump());
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
    for (const char* key : {"displayable_reasoning", "reasoning_summary"}) {
        if (result.contains(key) && result[key].is_string()) j[key] = result[key];
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
    const auto id=next_sse_event_id_++;
    auto chunk=std::string("id: ")+std::to_string(id)+"\ndata: "+json_payload+"\n\n";
    sse_chunks_.push_back(chunk);
    sse_replay_.emplace_back(id,std::move(chunk));
    constexpr std::size_t kReplayLimit=4096;
    while(sse_replay_.size()>kReplayLimit)sse_replay_.pop_front();
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

std::uint64_t WebHandler::subscribe_sse(std::uint64_t last_event_id) const {
    std::lock_guard<std::mutex> lock(sse_mutex_);
    if(last_event_id)return last_event_id+1;
    return sse_replay_.empty()?next_sse_event_id_:sse_replay_.front().first;
}

bool WebHandler::try_read_sse(std::uint64_t& cursor,std::string& out) const {
    std::lock_guard<std::mutex> lock(sse_mutex_);
    if(!sse_replay_.empty()&&cursor<sse_replay_.front().first)cursor=sse_replay_.front().first;
    const auto it=std::find_if(sse_replay_.begin(),sse_replay_.end(),[&](const auto& item){return item.first>=cursor;});
    if(it==sse_replay_.end())return false;
    out=it->second;cursor=it->first+1;return true;
}

void WebHandler::send_ws_message(const json& /*message*/) {}

bool WebHandler::check_connection() const {
    return connection_ && connection_->is_active;
}

} // namespace agent_framework
