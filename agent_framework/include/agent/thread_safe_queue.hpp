/**
 * @file thread_safe_queue.hpp
 * @brief WP2.U：线程安全队列（ImGui 等跨线程 UI投递）
 */
#ifndef __AGENT_THREAD_SAFE_QUEUE_H__
#define __AGENT_THREAD_SAFE_QUEUE_H__

#include <deque>
#include <mutex>
#include <optional>

namespace agent_framework {

template<typename T>
class ThreadSafeQueue {
public:
    ThreadSafeQueue() = default;

    void push(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(value));
    }

    bool try_pop(T& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return false;
        }
        out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return std::nullopt;
        }
        T v = std::move(queue_.front());
        queue_.pop_front();
        return v;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::deque<T> queue_;
};

} // namespace agent_framework

#endif // __AGENT_THREAD_SAFE_QUEUE_H__
