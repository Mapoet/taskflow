/**
 * @file sse_server_channel.cpp
 */
#include <agent/internal/sse_server_channel.hpp>

namespace agent_framework {
namespace internal {

SseServerChannel::SseServerChannel(std::size_t max_pending,
                                   std::size_t max_dropped_before_close)
    : max_pending_(max_pending == 0 ? 1 : max_pending),
      max_dropped_before_close_(max_dropped_before_close) {}

SseServerChannel::PushResult SseServerChannel::push_framed(std::string data) {
    std::lock_guard<std::mutex> lock(mu_);
    if (closed_.load(std::memory_order_acquire)) return PushResult::closed;
    PushResult result = PushResult::accepted;
    if (pending_.size() >= max_pending_) {
        pending_.pop_front();
        ++dropped_;
        result = PushResult::dropped_oldest;
        if (max_dropped_before_close_ > 0 && dropped_ >= max_dropped_before_close_) {
            closed_.store(true, std::memory_order_release);
        }
    }
    pending_.push_back(std::move(data));
    cv_.notify_one();
    return result;
}

std::size_t SseServerChannel::pending_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return pending_.size();
}

std::size_t SseServerChannel::dropped_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return dropped_;
}

void SseServerChannel::close() {
    closed_.store(true, std::memory_order_release);
    cv_.notify_all();
}

SseServerChannel::PopResult SseServerChannel::pop_or_wait(std::string& out,
                                                          std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mu_);
    const bool got = cv_.wait_for(lock, timeout, [this] {
        return closed_.load(std::memory_order_acquire) || !pending_.empty();
    });
    if (!pending_.empty()) {
        out = std::move(pending_.front());
        pending_.pop_front();
        return PopResult::chunk;
    }
    if (closed_.load(std::memory_order_acquire)) {
        return PopResult::closed;
    }
    if (!got) {
        return PopResult::timeout;
    }
    return PopResult::closed;
}

} // namespace internal
} // namespace agent_framework
