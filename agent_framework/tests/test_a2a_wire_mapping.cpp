/**
 * @file test_a2a_wire_mapping.cpp
 * @brief W-1..W-3（phase-2-wp1.md §8.3）
 */
#include <agent/a2a/wire_mapping.hpp>

#include <agent/core/types.hpp>

#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace af = agent_framework;
namespace a2a = af::a2a;

#ifndef AGENT_TEST_A2A_FIXTURE_DIR
#define AGENT_TEST_A2A_FIXTURE_DIR "."
#endif

static void w1_task_round_trip() {
    af::AgentTask t;
    t.task_id = "t-1";
    t.session_id = "sess";
    t.status = af::AgentTaskStatus::WORKING;
    t.updated_at = std::chrono::system_clock::now();
    t.metadata = json::object();
    json w = a2a::task_to_a2a_wire(t);
    af::AgentTask t2 = a2a::task_from_a2a_wire(w);
    if (t2.task_id != "t-1") {
        throw std::runtime_error("w1 task_id");
    }
    if (t2.session_id != "sess") {
        throw std::runtime_error("w1 session");
    }
    if (t2.status != af::AgentTaskStatus::WORKING) {
        throw std::runtime_error("w1 status");
    }
}

static void w2_fixture_from_wire() {
    std::string path = std::string(AGENT_TEST_A2A_FIXTURE_DIR) + "/tests/fixtures/a2a/task_min_v1.json";
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("missing fixture: " + path);
    }
    std::ostringstream oss;
    oss << in.rdbuf();
    json j = json::parse(oss.str());
    af::AgentTask t = a2a::task_from_a2a_wire(j);
    if (t.task_id != "task-fixture-1") {
        throw std::runtime_error("w2 id");
    }
    if (t.session_id != "ctx-99") {
        throw std::runtime_error("w2 context");
    }
    if (t.status != af::AgentTaskStatus::WORKING) {
        throw std::runtime_error("w2 state");
    }
    if (t.messages.size() != 1u) {
        throw std::runtime_error("w2 history");
    }
}

static void w3_sse_status_update() {
    json payload = json::parse(R"({
      "statusUpdate": {
        "taskId": "tid-sse",
        "contextId": "c1",
        "status": {
          "state": "TASK_STATE_COMPLETED",
          "timestamp": "2026-04-05T12:00:00.000Z"
        }
      }
    })");
    std::string buf;
    a2a::append_sse_event(buf, "", payload.dump());
    a2a::SseParser p;
    p.feed(buf);
    std::vector<a2a::SseEvent> evs;
    p.drain_events(evs);
    if (evs.size() != 1u) {
        throw std::runtime_error("w3 ev count");
    }
    af::AgentTask out;
    if (!a2a::try_parse_task_status_sse(evs[0], out)) {
        throw std::runtime_error("w3 parse");
    }
    if (out.task_id != "tid-sse") {
        throw std::runtime_error("w3 task id");
    }
    if (out.status != af::AgentTaskStatus::COMPLETED) {
        throw std::runtime_error("w3 status");
    }
}

int main() {
    w1_task_round_trip();
    w2_fixture_from_wire();
    w3_sse_status_update();
    std::cout << "test_a2a_wire_mapping: ok\n";
    return 0;
}
