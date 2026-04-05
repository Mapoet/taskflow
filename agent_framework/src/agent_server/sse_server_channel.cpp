/**
 * @file sse_server_channel.cpp
 */
#include <agent/internal/sse_server_channel.hpp>

namespace agent_framework {
namespace internal {

void SseServerChannel::push_framed(std::string data) {
    std::lock_guard<std::mutex> lock(mu_);
    pending_.push_back(std::move(data));
    cv_.notify_one();
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
