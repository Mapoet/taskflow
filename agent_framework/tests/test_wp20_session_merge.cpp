/**
 * @file test_wp20_session_merge.cpp
 * @brief WP2.0 merge_react_session_state 单元测试（phase-2-wp0.md §7.1）
 */

#include <agent/graph_executor.hpp>
#include <agent/internal/agent_thread_state.hpp>
#include <agent/types.hpp>

#include <cassert>
#include <iostream>
#include <memory>
#include <string>

namespace {

using namespace agent_framework;

void test_m1_empty_old_one_assistant() {
    internal::AgentThreadState S;
    auto next = std::make_shared<internal::AgentThreadState>();
    Message a;
    a.role = "assistant";
    a.content = "A";
    a.timestamp = 1;
    next->history.push_back(a);

    assert(merge_react_session_state(S, "hi", next));
    assert(S.history.size() == 2U);
    assert(S.history[0].role == "user");
    assert(S.history[0].content == "hi");
    assert(S.history[1].role == "assistant");
    assert(S.history[1].content == "A");
    assert(S.initial_user_prompt.empty());
}

void test_m2_prefix_then_delta() {
    internal::AgentThreadState S;
    Message u0;
    u0.role = "user";
    u0.content = "u0";
    u0.timestamp = 1;
    Message a0;
    a0.role = "assistant";
    a0.content = "A";
    a0.timestamp = 2;
    S.history.push_back(u0);
    S.history.push_back(a0);
    std::vector<Message> old_copy = S.history;

    auto next = std::make_shared<internal::AgentThreadState>();
    next->history = old_copy;
    Message t;
    t.role = "tool";
    t.content = "";
    t.tool_name = "t";
    t.tool_result = nlohmann::json{{"ok", true}};
    t.timestamp = 3;
    Message a2;
    a2.role = "assistant";
    a2.content = "A2";
    a2.timestamp = 4;
    next->history.push_back(t);
    next->history.push_back(a2);
    next->iteration = 2;

    assert(merge_react_session_state(S, "q2", next));
    assert(S.history.size() == old_copy.size() + 1U + 2U);
    assert(S.history[0].content == "u0");
    assert(S.history[1].content == "A");
    assert(S.history[2].role == "user");
    assert(S.history[2].content == "q2");
    assert(S.history[3].role == "tool");
    assert(S.history[4].content == "A2");
    assert(S.iteration == 2);
}

void test_m3_prefix_mismatch_no_touch() {
    internal::AgentThreadState S;
    Message u0;
    u0.role = "user";
    u0.content = "u0";
    u0.timestamp = 1;
    S.history.push_back(u0);
    std::vector<Message> before = S.history;

    auto next = std::make_shared<internal::AgentThreadState>();
    Message wrong;
    wrong.role = "user";
    wrong.content = "other";
    wrong.timestamp = 2;
    next->history.push_back(wrong);

    assert(!merge_react_session_state(S, "q", next));
    assert(S.history.size() == before.size());
    assert(S.history[0].content == "u0");
}

void test_m4_empty_user_skips_user_bubble() {
    internal::AgentThreadState S;
    auto next = std::make_shared<internal::AgentThreadState>();
    Message a;
    a.role = "assistant";
    a.content = "A";
    a.timestamp = 1;
    next->history.push_back(a);

    assert(merge_react_session_state(S, "", next));
    assert(S.history.size() == 1U);
    assert(S.history[0].role == "assistant");
    assert(S.history[0].content == "A");
    assert(S.initial_user_prompt.empty());
}

void test_m5_delta_only_no_second_user() {
    internal::AgentThreadState S;
    Message u0;
    u0.role = "user";
    u0.content = "u0";
    u0.timestamp = 1;
    Message a0;
    a0.role = "assistant";
    a0.content = "A0";
    a0.timestamp = 2;
    S.history.push_back(u0);
    S.history.push_back(a0);

    auto next = std::make_shared<internal::AgentThreadState>();
    next->history = S.history;
    Message a1;
    a1.role = "assistant";
    a1.content = "A1";
    a1.timestamp = 3;
    next->history.push_back(a1);

    assert(merge_react_session_state(S, "ignored_snapshot", next, MergeReactSessionMode::DeltaOnly));
    assert(S.history.size() == 3U);
    assert(S.history[0].role == "user");
    assert(S.history[1].content == "A0");
    assert(S.history[2].content == "A1");
}

} // namespace

int main() {
    test_m1_empty_old_one_assistant();
    test_m2_prefix_then_delta();
    test_m3_prefix_mismatch_no_touch();
    test_m4_empty_user_skips_user_bubble();
    test_m5_delta_only_no_second_user();
    std::cout << "test_wp20_session_merge: all passed\n";
    return 0;
}
