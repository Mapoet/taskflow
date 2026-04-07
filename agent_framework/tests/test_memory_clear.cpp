/**
 * @file test_memory_clear.cpp
 * @brief WP2.9 CL-1
 */

#include <agent/internal/agent_thread_state.hpp>
#include <agent/user_input_preprocessor.hpp>

#include <cassert>
#include <vector>

namespace {

using namespace agent_framework;

void test_cl1_clear_preserves_initial_prompt() {
    internal::AgentThreadState st;
    st.initial_user_prompt = "keep-this";
    st.iteration = 4;
    st.history.emplace_back();
    st.history.back().role = "user";
    st.history.back().content = "x";
    st.verifier_retry_count = 2;
    st.skill_prompt_cache = "sk";
    st.active_skill_id = "id";

    std::vector<ControlAction> actions;
    ControlAction a;
    a.command = "memory.clear";
    actions.push_back(std::move(a));
    dispatch_pending_control_actions(actions, nullptr, &st, nullptr, nullptr);

    assert(st.history.empty());
    assert(st.iteration == 0);
    assert(st.initial_user_prompt == "keep-this");
    assert(st.verifier_retry_count == 0);
    assert(!st.skill_prompt_cache.has_value());
    assert(!st.active_skill_id.has_value());
    assert(st.last_memory_auto_compact_iteration == -1);
}

} // namespace

int main() {
    test_cl1_clear_preserves_initial_prompt();
    return 0;
}
