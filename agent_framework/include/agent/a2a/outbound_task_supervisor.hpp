/**
 * @file outbound_task_supervisor.hpp
 * @brief WP2.agents: concurrent outbound A2A tasks, monitor, cancel, extend
 */
#ifndef __AGENT_A2A_OUTBOUND_TASK_SUPERVISOR_H__
#define __AGENT_A2A_OUTBOUND_TASK_SUPERVISOR_H__

#include <agent/a2a/orchestration.hpp>
#include <agent/a2a/peer_registry.hpp>
#include <agent/core/types.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>

namespace agent_framework {
namespace a2a {

inline constexpr const char* kA2aToolSubmitTask = "a2a_submit_task";
inline constexpr const char* kA2aToolGetTaskStatus = "a2a_get_task_status";
inline constexpr const char* kA2aToolWaitTasks = "a2a_wait_tasks";
inline constexpr const char* kA2aToolCancelTask = "a2a_cancel_task";
inline constexpr const char* kA2aToolExtendTimeout = "a2a_extend_task_timeout";
inline constexpr const char* kA2aToolListSubtasks = "a2a_list_subtasks";

struct OutboundSessionPolicy {
    bool cancel_all_on_new_user_turn = false;
    int max_parallel_a2a_submits = 4;
    /** Cap concurrent background monitors (plan §10). */
    int max_concurrent_monitors = 32;
};

/**
 * @brief Process-local supervisor for multiple remote tasks (one orchestrator session).
 */
class OutboundTaskSupervisor : public std::enable_shared_from_this<OutboundTaskSupervisor> {
public:
    OutboundTaskSupervisor(A2aPeerRegistry& registry,
                           PeerSessionBook& session_book,
                           OutboundSessionPolicy policy = {});

    ~OutboundTaskSupervisor();

    void set_remote_log(std::function<void(std::string_view peer_id, std::string_view line)> fn);
    void set_structured_log(std::function<void(const json&)> fn);

    A2aPeerRegistry& registry() { return registry_; }
    PeerSessionBook& session_book() { return session_book_; }
    const OutboundSessionPolicy& policy() const { return policy_; }

    /** @brief Submit SendMessage; optional background monitor. */
    json tool_submit(const json& args, const A2aToolRegistrationOptions& reg_opts);

    json tool_get_status(const json& args);
    json tool_wait_tasks(const json& args);
    json tool_cancel(const json& args);
    json tool_extend(const json& args);
    json tool_list_subtasks(const json& args);

    void on_user_turn_barrier();

    /** @brief Recent events + active handles for LLM digest (WP2.1c cap applied by caller). */
    std::string format_digest_for_llm(std::size_t max_events, std::size_t max_bytes) const;

    json debug_subtasks_json() const;

private:
    struct TrackedEntry {
        std::string local_handle;
        std::string peer_id;
        std::string remote_task_id;
        std::mutex mu;
        AgentTask snapshot{};
        std::chrono::steady_clock::time_point deadline{};
        std::atomic<bool> force_stop{false};
        std::optional<std::jthread> monitor_jt{};
    };

    void push_event_locked(std::unique_lock<std::mutex>& lk, json event);
    void emit_subtask_line(const std::string& peer_id,
                           const std::string& local_handle,
                           const std::string& remote_task_id,
                           const std::string& status,
                           const std::string& outcome);
    std::shared_ptr<TrackedEntry> find_by_handle(const std::string& h);
    std::shared_ptr<TrackedEntry> find_by_remote(const std::string& peer_id,
                                                 const std::string& task_id);

    A2aPeerRegistry& registry_;
    PeerSessionBook& session_book_;
    OutboundSessionPolicy policy_;
    std::function<void(std::string_view, std::string_view)> remote_log_;
    std::function<void(const json&)> structured_log_;

    mutable std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<TrackedEntry>> by_handle_;
    std::unordered_map<std::string, std::weak_ptr<TrackedEntry>> by_remote_;
    std::atomic<std::uint64_t> next_handle_{1};
    std::atomic<int> active_monitors_{0};

    std::deque<json> event_ring_;
    std::atomic<std::uint64_t> event_seq_{0};
    static constexpr std::size_t kDefaultRingCap = 256;
    std::size_t ring_cap_ = kDefaultRingCap;
};

} // namespace a2a
} // namespace agent_framework

#endif // __AGENT_A2A_OUTBOUND_TASK_SUPERVISOR_H__
