/**
 * @file sse_server_channel.hpp
 * @brief 单路 SSE 订阅缓冲（WP2.2 httplib chunked provider）
 * @author Mapoet
 * @version 0.1
 * @date 2026-04-05
 */
#ifndef __AGENT_INTERNAL_SSE_SERVER_CHANNEL_H__
#define __AGENT_INTERNAL_SSE_SERVER_CHANNEL_H__

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <cstddef>
#include <string>

namespace agent_framework {
namespace internal {

/**
 * @brief 已含完整 SSE 事件 UTF-8 块（含结尾空行）的队列
 */
class SseServerChannel {
public:
    enum class PushResult { accepted, dropped_oldest, closed };
    explicit SseServerChannel(std::size_t max_pending = 256,
                              std::size_t max_dropped_before_close = 1024);
    PushResult push_framed(std::string data);
    void close();

    enum class PopResult {
        chunk,
        timeout,
        closed
    };

    /** @brief 等待一条完整帧或超时；closed 且无数据时返回 closed */
    PopResult pop_or_wait(std::string& out, std::chrono::milliseconds timeout);

    bool is_closed() const { return closed_.load(std::memory_order_acquire); }
    std::size_t pending_count() const;
    std::size_t dropped_count() const;
    std::size_t capacity() const { return max_pending_; }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::string> pending_;
    std::atomic<bool> closed_{false};
    const std::size_t max_pending_;
    const std::size_t max_dropped_before_close_;
    std::size_t dropped_ = 0;
};

} // namespace internal
} // namespace agent_framework

#endif // __AGENT_INTERNAL_SSE_SERVER_CHANNEL_H__
