#pragma once
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include "agent/tool_runtime/store.hpp"

namespace agent_framework::tool_runtime
{
enum class InvocationSubscriptionRead { Event, Timeout, Closed, Overflow, CursorExpired, IntegrityFailure };

class InvocationEventSubscription
{
public:
    InvocationSubscriptionRead next(InvocationEvent &, std::chrono::milliseconds);
    void close();
    std::uint64_t cursor() const;
    bool overflowed() const;
private:
    friend class InvocationEventStreamHub;
    struct PullState;
    InvocationEventSubscription(std::string, std::uint64_t, std::size_t,
                                std::shared_ptr<PullState>, std::chrono::milliseconds);
    void offer(const InvocationEvent &);
    InvocationSubscriptionRead pull_from_store();
    std::string invocation_id_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<InvocationEvent> pending_;
    std::uint64_t cursor_{0};
    std::size_t capacity_{0};
    bool closed_{false}, overflowed_{false};
    InvocationSubscriptionRead terminal_{InvocationSubscriptionRead::Closed};
    std::shared_ptr<PullState> pull_state_;
    std::chrono::milliseconds poll_interval_{100};
};

struct InvocationSubscribeResult
{
    std::shared_ptr<InvocationEventSubscription> subscription;
    std::uint64_t head_sequence{0};
    std::string error;
};

class InvocationEventStreamHub
{
public:
    explicit InvocationEventStreamHub(InvocationStore &);
    ~InvocationEventStreamHub();
    InvocationSubscribeResult subscribe(std::string_view, std::uint64_t after = 0,
                                        std::size_t capacity = 256);
    void publish(const InvocationEvent &);
    void close_all();
private:
    void prune_locked(const std::string &);
    InvocationStore &store_;
    std::shared_ptr<InvocationEventSubscription::PullState> pull_state_;
    std::mutex mutex_;
    std::unordered_map<std::string, std::vector<std::weak_ptr<InvocationEventSubscription>>> subscribers_;
};
}
