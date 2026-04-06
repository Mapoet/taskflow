/**
 * @file a2a_task_monitor.cpp
 * @brief SSE/poll monitor for remote A2A tasks
 */
#include <agent/a2a/a2a_task_monitor.hpp>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>

namespace agent_framework {
namespace a2a {

bool a2a_task_is_terminal(AgentTaskStatus s) {
    return s == AgentTaskStatus::COMPLETED || s == AgentTaskStatus::FAILED ||
           s == AgentTaskStatus::CANCELLED || s == AgentTaskStatus::INPUT_REQUIRED;
}

namespace {

const char* task_status_cstr(AgentTaskStatus s) {
    switch (s) {
    case AgentTaskStatus::PENDING:
        return "pending";
    case AgentTaskStatus::WORKING:
        return "working";
    case AgentTaskStatus::COMPLETED:
        return "completed";
    case AgentTaskStatus::FAILED:
        return "failed";
    case AgentTaskStatus::INPUT_REQUIRED:
        return "input_required";
    case AgentTaskStatus::CANCELLED:
        return "cancelled";
    }
    return "unknown";
}

} // namespace

A2aTaskMonitorOutcome monitor_remote_task_until_deadline(
    const std::string& peer_id,
    AgentClient& rpc_client,
    const AgentCard& card,
    const AgentTask& task_after_send,
    const A2aRemoteTaskOptions& opts,
    std::function<std::chrono::steady_clock::time_point()> deadline_supplier) {

    A2aTaskMonitorOutcome out;
    out.latest = task_after_send;

    const bool use_sse = opts.use_sse_if_capable && agent_card_has_streaming(card);

    if (use_sse) {
        std::mutex mu;
        std::condition_variable cv;
        bool terminal = a2a_task_is_terminal(out.latest.status);

        auto on_status = [&](const AgentTask& t) {
            {
                std::lock_guard<std::mutex> lk(mu);
                out.latest = t;
            }
            if (opts.on_remote_log) {
                std::ostringstream line;
                line << "task_id=" << t.task_id << " status=" << task_status_cstr(t.status);
                opts.on_remote_log(peer_id, line.str());
            }
            if (a2a_task_is_terminal(t.status)) {
                terminal = true;
                cv.notify_all();
            }
        };
        rpc_client.subscribe_task_updates("", task_after_send.task_id, on_status,
                                          [](const AgentArtifact&) {});

        std::unique_lock<std::mutex> ul(mu);
        while (!terminal && std::chrono::steady_clock::now() < deadline_supplier()) {
            const auto dl = deadline_supplier();
            const auto left = dl - std::chrono::steady_clock::now();
            if (left <= std::chrono::steady_clock::duration::zero()) {
                break;
            }
            cv.wait_for(ul, left, [&] { return terminal; });
        }
        ul.unlock();

        if (!terminal) {
            try {
                out.latest = rpc_client.get_task("", task_after_send.task_id).get();
            } catch (...) {
                // keep latest
            }
        }
    } else {
        while (!a2a_task_is_terminal(out.latest.status) &&
               std::chrono::steady_clock::now() < deadline_supplier()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            try {
                out.latest = rpc_client.get_task("", task_after_send.task_id).get();
            } catch (const std::exception& e) {
                out.poll_error = e.what();
                return out;
            }
            if (opts.on_remote_log) {
                std::ostringstream line;
                line << "task_id=" << out.latest.task_id << " status="
                     << task_status_cstr(out.latest.status);
                opts.on_remote_log(peer_id, line.str());
            }
        }
    }

    return out;
}

} // namespace a2a
} // namespace agent_framework
