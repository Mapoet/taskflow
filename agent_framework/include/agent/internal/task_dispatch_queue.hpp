/**
 * @file task_dispatch_queue.hpp
 * @brief 有界任务队列（WP2.2 AgentServer worker 池）
 * @author Mapoet
 * @version 0.1
 * @date 2026-04-05
 */
#ifndef __AGENT_INTERNAL_TASK_DISPATCH_QUEUE_H__
#define __AGENT_INTERNAL_TASK_DISPATCH_QUEUE_H__

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>

namespace agent_framework {
namespace internal {

/**
 * @brief 有界 FIFO；shutdown 后 wait_pop 返回 false
 */
class TaskDispatchQueue {
public:
    explicit TaskDispatchQueue(std::size_t max_queued);

    TaskDispatchQueue(const TaskDispatchQueue&) = delete;
    TaskDispatchQueue& operator=(const TaskDispatchQueue&) = delete;

    /** @return false 若已 shutdown 或队列已满 */
    bool try_push(std::function<void()> job);

    /** @return false 若 shutdown 且队列已空 */
    bool wait_pop(std::function<void()>& job);

    void shutdown();

    std::size_t max_queued() const { return max_queued_; }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    std::size_t max_queued_;
    bool shutdown_{false};
};

} // namespace internal
} // namespace agent_framework

#endif // __AGENT_INTERNAL_TASK_DISPATCH_QUEUE_H__
