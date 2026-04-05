/**
 * @file task_dispatch_queue.cpp
 * @brief 有界任务队列实现
 */
#include <agent/internal/task_dispatch_queue.hpp>

namespace agent_framework {
namespace internal {

TaskDispatchQueue::TaskDispatchQueue(std::size_t max_queued) : max_queued_(max_queued == 0 ? 1 : max_queued) {}

bool TaskDispatchQueue::try_push(std::function<void()> job) {
    std::lock_guard<std::mutex> lock(mu_);
    if (shutdown_) {
        return false;
    }
    if (q_.size() >= max_queued_) {
        return false;
    }
    q_.push_back(std::move(job));
    cv_.notify_one();
    return true;
}

bool TaskDispatchQueue::wait_pop(std::function<void()>& job) {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this] { return shutdown_ || !q_.empty(); });
    if (q_.empty()) {
        return false;
    }
    job = std::move(q_.front());
    q_.pop_front();
    cv_.notify_one();
    return true;
}

void TaskDispatchQueue::shutdown() {
    std::lock_guard<std::mutex> lock(mu_);
    shutdown_ = true;
    cv_.notify_all();
}

} // namespace internal
} // namespace agent_framework
