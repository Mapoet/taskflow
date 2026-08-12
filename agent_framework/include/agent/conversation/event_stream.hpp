#pragma once

#include <condition_variable>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include "agent/conversation/store.hpp"

namespace agent_framework::conversation
{
    enum class SubscriptionRead
    {
        Event,
        Timeout,
        Closed,
        Overflow
    };

    class EventSubscription
    {
    public:
        SubscriptionRead next(RuntimeEventEnvelope &, std::chrono::milliseconds timeout);
        void close();
        std::uint64_t cursor() const;
        bool overflowed() const;

    private:
        friend class EventStreamHub;
        EventSubscription(ConversationIdentity, std::uint64_t, std::size_t);
        void offer(const RuntimeEventEnvelope &);
        ConversationIdentity identity_;
        mutable std::mutex mutex_;
        std::condition_variable ready_;
        std::deque<RuntimeEventEnvelope> pending_;
        std::uint64_t cursor_{0};
        std::size_t capacity_{0};
        bool closed_{false};
        bool overflowed_{false};
    };

    struct SubscribeResult
    {
        std::shared_ptr<EventSubscription> subscription;
        std::uint64_t head_sequence{0};
        std::string error;
    };

    class EventStreamHub
    {
    public:
        explicit EventStreamHub(ConversationStore &store) : store_(store) {}
        SubscribeResult subscribe(const ConversationIdentity &, std::uint64_t after = 0,
                                  std::size_t capacity = 256);
        void publish(const RuntimeEventEnvelope &);
        void close_all();

    private:
        static std::string key(const ConversationIdentity &);
        void prune_locked(const std::string &);
        ConversationStore &store_;
        std::mutex mutex_;
        std::unordered_map<std::string, std::vector<std::weak_ptr<EventSubscription>>> subscribers_;
    };
}
