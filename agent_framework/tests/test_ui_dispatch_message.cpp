/**
 * @file test_ui_dispatch_message.cpp
 * @brief WP2.U U-1：UIManager::dispatch_message → handle_aux_event
 */

#include <agent/ui/ui_manager.hpp>

#include <cassert>
#include <string>
#include <utility>

namespace {

using namespace agent_framework;

class RecordingHandler final : public UIHandler {
public:
    int aux_count = 0;
    std::string last_type;
    json last_payload;

    void handle_stream_token(std::string_view) override {}
    void handle_final_result(const json&) override {}
    void handle_error(const std::string&) override {}
    std::string get_handler_type() const override {
        return "recording";
    }
    bool is_active() const override {
        return true;
    }
    void handle_aux_event(std::string_view type, const json& payload) override {
        ++aux_count;
        last_type = std::string(type);
        last_payload = payload;
    }
};

void test_u1_dispatch_hits_all_handlers() {
    UIManager ui;
    auto a = std::make_unique<RecordingHandler>();
    RecordingHandler* pa = a.get();
    auto b = std::make_unique<RecordingHandler>();
    RecordingHandler* pb = b.get();
    ui.register_handler(std::move(a));
    ui.register_handler(std::move(b));

    json data = json{{"name", "fs_read"}, {"ok", true}};
    ui.dispatch_message("tool_end", data);

    assert(pa->aux_count == 1);
    assert(pb->aux_count == 1);
    assert(pa->last_type == "tool_end");
    assert(pa->last_payload == data);
}

void test_stream_isolated_by_session_and_channel() {
    UIManager ui;
    auto ca = std::make_shared<WebConnectionInfo>();
    ca->session_id = "a";
    ca->is_active = true;
    auto cb = std::make_shared<WebConnectionInfo>();
    cb->session_id = "b";
    cb->is_active = true;
    auto a = std::make_unique<WebHandler>("a", ca);
    auto b = std::make_unique<WebHandler>("b", cb);
    WebHandler* pa = a.get();
    WebHandler* pb = b.get();
    ui.register_web_connection("a", std::move(a));
    ui.register_web_connection("b", std::move(b));

    ui.stream_token("a", "answer");
    ui.stream_thinking("a", "summary");

    std::string chunk;
    assert(pa->try_pop_sse_chunk(chunk));
    assert(chunk.find("\"kind\":\"token\"") != std::string::npos);
    assert(chunk.find("answer") != std::string::npos);
    assert(pa->try_pop_sse_chunk(chunk));
    assert(chunk.find("\"kind\":\"thinking\"") != std::string::npos);
    assert(chunk.find("summary") != std::string::npos);
    assert(!pb->try_pop_sse_chunk(chunk));
}

} // namespace

int main() {
    test_u1_dispatch_hits_all_handlers();
    test_stream_isolated_by_session_and_channel();
    return 0;
}
