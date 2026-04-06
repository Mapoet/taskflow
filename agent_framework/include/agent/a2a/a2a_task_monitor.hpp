/**
 * @file a2a_task_monitor.hpp
 * @brief Shared SSE/poll wait until A2A task terminal or deadline (WP2.agents PR-1)
 */
#ifndef __AGENT_A2A_TASK_MONITOR_H__
#define __AGENT_A2A_TASK_MONITOR_H__

#include <agent/a2a/orchestration.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <string>

namespace agent_framework {
namespace a2a {

bool a2a_task_is_terminal(AgentTaskStatus s);

/**
 * @brief Poll or SSE until terminal state or steady_clock deadline; mirrors run_remote_task_and_wait core.
 * @return latest task snapshot; error set if polling loop get_task threw
 */
struct A2aTaskMonitorOutcome {
    AgentTask latest;
    std::optional<std::string> poll_error;
};

A2aTaskMonitorOutcome monitor_remote_task_until_deadline(
    const std::string& peer_id,
    AgentClient& rpc_client,
    const AgentCard& card,
    const AgentTask& task_after_send,
    const A2aRemoteTaskOptions& opts,
    std::function<std::chrono::steady_clock::time_point()> deadline_supplier);

inline A2aTaskMonitorOutcome monitor_remote_task_until_deadline(
    const std::string& peer_id,
    AgentClient& rpc_client,
    const AgentCard& card,
    const AgentTask& task_after_send,
    const A2aRemoteTaskOptions& opts,
    std::chrono::steady_clock::time_point deadline) {
    return monitor_remote_task_until_deadline(peer_id, rpc_client, card, task_after_send, opts,
                                              [deadline] { return deadline; });
}

} // namespace a2a
} // namespace agent_framework

#endif // __AGENT_A2A_TASK_MONITOR_H__
