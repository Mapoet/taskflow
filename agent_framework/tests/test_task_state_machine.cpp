/**
 * @file test_task_state_machine.cpp
 * @brief WP2.3 T-1 / T-2 / T-3 state machine + wire round-trip
 */
#include "agent/a2a/wire_mapping.hpp"
#include "agent/task_state_machine.hpp"
#include <iostream>
#include <string>

namespace {

using agent_framework::AgentTask;
using agent_framework::AgentTaskStatus;
using agent_framework::agent_task_status_cstr;
using agent_framework::a2a::agent_task_status_from_a2a_state;
using agent_framework::a2a::agent_task_status_to_a2a_state;
using agent_framework::try_transition;

bool expect(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << "\n";
        return false;
    }
    return true;
}

} // namespace

int main() {
    // T-1: PENDING -> WORKING -> COMPLETED
    {
        AgentTask t;
        t.status = AgentTaskStatus::PENDING;
        std::string err;
        if (!expect(try_transition(t, AgentTaskStatus::WORKING, &err), "T-1 PENDING->WORKING")) {
            std::cerr << err << "\n";
            return 1;
        }
        if (!expect(try_transition(t, AgentTaskStatus::COMPLETED, &err), "T-1 WORKING->COMPLETED")) {
            std::cerr << err << "\n";
            return 1;
        }
    }

    // T-2: COMPLETED -> WORKING must fail
    {
        AgentTask t;
        t.status = AgentTaskStatus::COMPLETED;
        std::string err;
        if (!expect(!try_transition(t, AgentTaskStatus::WORKING, &err), "T-2 should fail")) {
            return 1;
        }
        if (!expect(t.status == AgentTaskStatus::COMPLETED, "T-2 status unchanged")) {
            return 1;
        }
        if (!expect(err.find("illegal_transition") != std::string::npos, "T-2 err code")) {
            std::cerr << "got: " << err << "\n";
            return 1;
        }
        if (!expect(err.find("COMPLETED") != std::string::npos, "T-2 err from")) {
            return 1;
        }
        if (!expect(err.find("WORKING") != std::string::npos, "T-2 err to")) {
            return 1;
        }
    }

    // T-3: wire round-trip (symmetric TaskState enum names)
    {
        const AgentTaskStatus sts[] = {AgentTaskStatus::PENDING, AgentTaskStatus::WORKING,
                                       AgentTaskStatus::COMPLETED, AgentTaskStatus::FAILED,
                                       AgentTaskStatus::INPUT_REQUIRED, AgentTaskStatus::CANCELLED};
        for (AgentTaskStatus s : sts) {
            std::string w = agent_task_status_to_a2a_state(s);
            AgentTaskStatus back = agent_task_status_from_a2a_state(w);
            if (!expect(back == s, "T-3 round-trip")) {
                std::cerr << "wire=" << w << "\n";
                return 1;
            }
        }
    }

    (void)agent_task_status_cstr(AgentTaskStatus::PENDING);
    std::cout << "test_task_state_machine: ok\n";
    return 0;
}
