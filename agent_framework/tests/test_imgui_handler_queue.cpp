/**
 * @file test_imgui_handler_queue.cpp
 * @brief WP2.U U-2：ImGuiHandler 入队 + drain（无 GLFW）
 */

#include <agent/ui/thread_safe_queue.hpp>
#include <agent/ui/ui_manager.hpp>

#include <cassert>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace agent_framework;

void test_push_drain_order() {
    auto q = std::make_shared<ThreadSafeQueue<StreamMessage>>();
    ImGuiHandler h(q, "default");
    h.handle_stream_token("ab");
    h.handle_stream_token("c");
    std::vector<StreamMessage> out;
    const std::size_t n = h.drain_messages(out, 10);
    assert(n == 2);
    assert(out[0].message_type == "token");
    assert(out[0].content == "ab");
    assert(out[1].content == "c");
}

void test_aux_event_message_type() {
    auto q = std::make_shared<ThreadSafeQueue<StreamMessage>>();
    ImGuiHandler h(q, "default");
    json p = json{{"name", "t"}};
    h.handle_aux_event("tool_start", p);
    std::vector<StreamMessage> out;
    assert(h.drain_messages(out, 5) == 1);
    assert(out[0].message_type == "aux:tool_start");
    assert(out[0].content == p.dump());
}

void test_final_pushes_body_when_no_stream() {
    auto q = std::make_shared<ThreadSafeQueue<StreamMessage>>();
    ImGuiHandler h(q, "default");
    json r;
    r["final_answer"] = std::string("hello_no_stream_body");
    r["iteration"] = 0;
    h.handle_final_result(r);
    std::vector<StreamMessage> out;
    assert(h.drain_messages(out, 5) == 1);
    assert(out[0].message_type == "final");
    assert(out[0].content == "hello_no_stream_body");
}

void test_final_meta_only_when_streamed() {
    auto q = std::make_shared<ThreadSafeQueue<StreamMessage>>();
    ImGuiHandler h(q, "default");
    h.handle_stream_token("x");
    json r;
    r["final_answer"] = std::string("full");
    r["iteration"] = 1;
    h.handle_final_result(r);
    std::vector<StreamMessage> out;
    assert(h.drain_messages(out, 5) == 2);
    assert(out[0].message_type == "token");
    assert(out[0].content == "x");
    assert(out[1].message_type == "final");
    assert(out[1].content.find("final_answer_chars") != std::string::npos);
}

} // namespace

int main() {
    test_push_drain_order();
    test_aux_event_message_type();
    test_final_pushes_body_when_no_stream();
    test_final_meta_only_when_streamed();
    return 0;
}
