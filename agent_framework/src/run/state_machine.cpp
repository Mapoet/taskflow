#include "agent/run/state_machine.hpp"

namespace agent_framework::run {

bool is_terminal(RunState state) noexcept {
    return state == RunState::Completed || state == RunState::Partial ||
           state == RunState::Rejected || state == RunState::Failed ||
           state == RunState::Cancelled;
}

bool can_transition(RunState from, RunState to) noexcept {
    if(from == to) return !is_terminal(from);
    if(is_terminal(from)) return false;
    if(to == RunState::Cancelled || to == RunState::Failed) return true;
    switch(from) {
        case RunState::Created: return to == RunState::Received;
        case RunState::Received: return to == RunState::Planning || to == RunState::Running;
        case RunState::Planning:
            return to == RunState::AwaitingApproval || to == RunState::Running ||
                   to == RunState::Interrupted;
        case RunState::AwaitingApproval:
            return to == RunState::Running || to == RunState::Planning ||
                   to == RunState::Rejected || to == RunState::Interrupted;
        case RunState::Running:
            return to == RunState::Waiting || to == RunState::Interrupted ||
                   to == RunState::Verifying || to == RunState::Replanning;
        case RunState::Waiting:
            return to == RunState::Running || to == RunState::Interrupted;
        case RunState::Interrupted:
            return to == RunState::Planning || to == RunState::Running ||
                   to == RunState::Verifying || to == RunState::Replanning ||
                   to == RunState::Rejected;
        case RunState::Verifying:
            return to == RunState::Completed || to == RunState::Partial ||
                   to == RunState::Rejected || to == RunState::Replanning ||
                   to == RunState::Interrupted;
        case RunState::Replanning:
            return to == RunState::AwaitingApproval || to == RunState::Running ||
                   to == RunState::Interrupted;
        case RunState::Completed:
        case RunState::Partial:
        case RunState::Rejected:
        case RunState::Failed:
        case RunState::Cancelled: return false;
    }
    return false;
}

std::string run_state_name(RunState state) {
    static const char* names[] = {"created", "received", "planning", "awaiting_approval",
        "running", "waiting", "interrupted", "verifying", "replanning", "completed",
        "partial", "rejected", "failed", "cancelled"};
    return names[static_cast<std::size_t>(state)];
}

}  // namespace agent_framework::run
