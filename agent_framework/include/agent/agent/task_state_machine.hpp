/**
 * @file task_state_machine.hpp
 * @brief Agent task runtime: cooperative cancel/deadline (TaskControl) and legal
 *        AgentTask status transitions (WP2.3)
 * @author Mapoet
 * @version 0.1
 * @date 2026-04-05
 */
#ifndef __AGENT_TASK_STATE_MACHINE_H__
#define __AGENT_TASK_STATE_MACHINE_H__

#include <agent/core/types.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <memory>
#include <optional>
#include <string>

namespace agent_framework {

/**
 * @brief Per-task flags and deadline for AgentServer / AgentLoop (WP2.3).
 *
 * Cancel and deadline are observed cooperatively in worker and loop body.
 * Status transitions remain on AgentTask under tasks_mutex_ + try_transition.
 */
class TaskControl {
public:
    TaskControl() = default;

    TaskControl(const TaskControl&) = delete;
    TaskControl& operator=(const TaskControl&) = delete;

    void request_cancel() { cancel_requested_->store(true, std::memory_order_release); }

    bool is_cancel_requested() const {
        return cancel_requested_->load(std::memory_order_acquire);
    }

    std::shared_ptr<std::atomic_bool> cancellation_token() const noexcept {
        return cancel_requested_;
    }

    void mark_deadline_exceeded() { deadline_exceeded_.store(true, std::memory_order_release); }

    bool is_deadline_exceeded() const {
        return deadline_exceeded_.load(std::memory_order_acquire);
    }

    /**
     * @brief Arm deadline from steady_clock::now() + timeout_sec. sec<=0 clears deadline.
     */
    void arm_working_deadline(int timeout_sec);

    /** @brief If armed and now >= deadline, sets deadline_exceeded. */
    void check_deadline_now();
    std::optional<std::chrono::steady_clock::time_point> working_deadline() const;

    /** @brief Effective timeout in seconds (0 = none), set before WORKING. */
    void set_effective_timeout_sec(int sec) { effective_timeout_sec_ = sec; }

    int effective_timeout_sec() const { return effective_timeout_sec_; }

private:
    std::shared_ptr<std::atomic_bool> cancel_requested_ {
        std::make_shared<std::atomic_bool>(false)
    };
    std::atomic<bool> deadline_exceeded_{false};
    int effective_timeout_sec_{0};

    mutable std::mutex deadline_mu_;
    std::optional<std::chrono::steady_clock::time_point> working_deadline_;
};

bool try_transition(AgentTask& task, AgentTaskStatus to, std::string* err_out);

const char* agent_task_status_cstr(AgentTaskStatus s);

} // namespace agent_framework

#endif // __AGENT_TASK_STATE_MACHINE_H__
