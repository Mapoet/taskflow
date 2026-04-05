/**
 * @file test_a2a_sse_framing.cpp
 * @brief SSE-1..SSE-5（phase-2-wp1.md §8.2）
 */
#include <agent/a2a/sse_framing.hpp>

#include <cassert>
#include <iostream>
#include <string>

namespace af = agent_framework;
namespace a2a = af::a2a;

static void sse1_round_trip_single_data() {
    std::string buf;
    a2a::append_sse_event(buf, "test", R"({"x":1})");
    a2a::SseParser p;
    p.feed(buf);
    std::vector<a2a::SseEvent> evs;
    p.drain_events(evs);
    assert(evs.size() == 1u);
    assert(evs[0].event == "test");
    assert(evs[0].data == R"({"x":1})");
    assert(!evs[0].id.has_value());
}

static void sse2_split_feed() {
    std::string chunk1 = "event: e\n";
    std::string chunk2 = "data: hi\n\n";
    a2a::SseParser p;
    p.feed(chunk1);
    std::vector<a2a::SseEvent> evs;
    p.drain_events(evs);
    assert(evs.empty());
    p.feed(chunk2);
    p.drain_events(evs);
    assert(evs.size() == 1u);
    assert(evs[0].event == "e");
    assert(evs[0].data == "hi");
}

static void sse3_multi_data_lines() {
    std::string buf;
    a2a::append_sse_event(buf, "", "line1\nline2");
    a2a::SseParser p;
    p.feed(buf);
    std::vector<a2a::SseEvent> evs;
    p.drain_events(evs);
    assert(evs.size() == 1u);
    assert(evs[0].data == "line1\nline2");
}

static void sse4_json_with_escaped_newlines() {
    // JSON 字符串内含 \n 转义，不应被误认为事件结束
    std::string buf;
    a2a::append_sse_event(buf, "j", R"({"t":"a\nb"})");
    a2a::SseParser p;
    p.feed(buf);
    std::vector<a2a::SseEvent> evs;
    p.drain_events(evs);
    assert(evs.size() == 1u);
    assert(evs[0].data.find("a\\nb") != std::string::npos || evs[0].data.find("a\nb") != std::string::npos);
}

static void sse5_id_field() {
    std::string buf;
    a2a::append_sse_event(buf, "msg", "{}", std::string("evt-42"));
    a2a::SseParser p;
    p.feed(buf);
    std::vector<a2a::SseEvent> evs;
    p.drain_events(evs);
    assert(evs.size() == 1u);
    assert(evs[0].id.has_value());
    assert(*evs[0].id == "evt-42");
}

int main() {
    sse1_round_trip_single_data();
    sse2_split_feed();
    sse3_multi_data_lines();
    sse4_json_with_escaped_newlines();
    sse5_id_field();
    std::cout << "test_a2a_sse_framing: ok\n";
    return 0;
}
