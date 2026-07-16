/**
 * @file task_state_machine.cpp
 * @brief TaskControl deadline + AgentTask state machine (WP2.3)
 */
#include <agent/agent/task_state_machine.hpp>

#include <sstream>

namespace agent_framework {

void TaskControl::arm_working_deadline(int timeout_sec) {
    std::lock_guard<std::mutex> lk(deadline_mu_);
    if (timeout_sec <= 0) {
        working_deadline_.reset();
        return;
    }
    working_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
}

void TaskControl::check_deadline_now() {
    std::lock_guard<std::mutex> lk(deadline_mu_);
    if (!working_deadline_.has_value()) {
        return;
    }
    if (std::chrono::steady_clock::now() >= *working_deadline_) {
        deadline_exceeded_.store(true, std::memory_order_release);
    }
}

std::optional<std::chrono::steady_clock::time_point> TaskControl::working_deadline() const {
    std::lock_guard<std::mutex> lk(deadline_mu_);
    return working_deadline_;
}

const char* agent_task_status_cstr(AgentTaskStatus s) {
    switch (s) {
    case AgentTaskStatus::PENDING:
        return "PENDING";
    case AgentTaskStatus::WORKING:
        return "WORKING";
    case AgentTaskStatus::COMPLETED:
        return "COMPLETED";
    case AgentTaskStatus::FAILED:
        return "FAILED";
    case AgentTaskStatus::INPUT_REQUIRED:
        return "INPUT_REQUIRED";
    case AgentTaskStatus::CANCELLED:
        return "CANCELLED";
    }
    return "UNKNOWN";
}

namespace {

bool is_allowed_transition(AgentTaskStatus from, AgentTaskStatus to) {
    if (from == to) {
        return true;
    }
    switch (from) {
    case AgentTaskStatus::PENDING:
        return to == AgentTaskStatus::WORKING || to == AgentTaskStatus::CANCELLED;
    case AgentTaskStatus::WORKING:
        return to == AgentTaskStatus::COMPLETED || to == AgentTaskStatus::FAILED ||
               to == AgentTaskStatus::CANCELLED || to == AgentTaskStatus::INPUT_REQUIRED;
    case AgentTaskStatus::INPUT_REQUIRED:
        return to == AgentTaskStatus::WORKING || to == AgentTaskStatus::CANCELLED;
    default:
        return false;
    }
}

} // namespace

bool try_transition(AgentTask& task, AgentTaskStatus to, std::string* err_out) {
    const AgentTaskStatus from = task.status;
    if (from == to) {
        return true;
    }
    if (!is_allowed_transition(from, to)) {
        if (err_out) {
            std::ostringstream os;
            os << "illegal_transition from=" << agent_task_status_cstr(from)
               << " to=" << agent_task_status_cstr(to);
            *err_out = os.str();
        }
        return false;
    }
    task.status = to;
    task.updated_at = std::chrono::system_clock::now();
    return true;
}

} // namespace agent_framework
