#include "agent/conversation/event_stream.hpp"
#include <algorithm>

namespace agent_framework::conversation
{
    EventSubscription::EventSubscription(ConversationIdentity identity, std::uint64_t cursor,
                                         std::size_t capacity)
        : identity_(std::move(identity)), cursor_(cursor), capacity_(std::max<std::size_t>(1, capacity)) {}

    void EventSubscription::offer(const RuntimeEventEnvelope &event)
    {
        std::lock_guard lock(mutex_);
        if (closed_ || event.sequence <= cursor_)
            return;
        if (pending_.size() >= capacity_)
        {
            overflowed_ = true;
            closed_ = true;
            ready_.notify_all();
            return;
        }
        pending_.push_back(event);
        ready_.notify_one();
    }

    SubscriptionRead EventSubscription::next(RuntimeEventEnvelope &event,
                                               std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        ready_.wait_for(lock, timeout, [&] { return closed_ || !pending_.empty(); });
        if (!pending_.empty())
        {
            event = std::move(pending_.front());
            pending_.pop_front();
            cursor_ = std::max(cursor_, event.sequence);
            return SubscriptionRead::Event;
        }
        if (overflowed_)
            return SubscriptionRead::Overflow;
        return closed_ ? SubscriptionRead::Closed : SubscriptionRead::Timeout;
    }

    void EventSubscription::close()
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        ready_.notify_all();
    }

    std::uint64_t EventSubscription::cursor() const
    {
        std::lock_guard lock(mutex_);
        return cursor_;
    }

    bool EventSubscription::overflowed() const
    {
        std::lock_guard lock(mutex_);
        return overflowed_;
    }

    std::string EventStreamHub::key(const ConversationIdentity &identity)
    {
        return identity.tenant_id + "\x1f" + identity.conversation_id;
    }

    SubscribeResult EventStreamHub::subscribe(const ConversationIdentity &identity,
                                               std::uint64_t after, std::size_t capacity)
    {
        std::lock_guard lock(mutex_);
        const auto head = store_.last_event_sequence(identity);
        const auto floor = store_.event_retention_floor(identity);
        if (after > head)
            return {{}, head, "cursor_ahead_of_head"};
        if (floor > 0 && after + 1 < floor)
            return {{}, head, "cursor_expired"};
        auto replay = store_.events(identity, after);
        if (head > after && replay.empty())
            return {{}, head, "event_replay_integrity_failure"};
        if (replay.size() > std::max<std::size_t>(1, capacity))
            return {{}, head, "replay_exceeds_capacity"};
        auto subscription = std::shared_ptr<EventSubscription>(
            new EventSubscription(identity, after, capacity));
        for (const auto &event : replay)
            subscription->offer(event);
        subscribers_[key(identity)].push_back(subscription);
        return {std::move(subscription), head, {}};
    }

    void EventStreamHub::prune_locked(const std::string &stream)
    {
        auto found = subscribers_.find(stream);
        if (found == subscribers_.end())
            return;
        auto &list = found->second;
        list.erase(std::remove_if(list.begin(), list.end(), [](const auto &weak) {
            auto subscription = weak.lock();
            return !subscription || subscription->overflowed();
        }), list.end());
        if (list.empty())
            subscribers_.erase(found);
    }

    void EventStreamHub::publish(const RuntimeEventEnvelope &event)
    {
        if (event.durability != EventDurability::Durable || event.sequence == 0)
            return;
        std::lock_guard lock(mutex_);
        const std::string stream = key({event.tenant_id, event.conversation_id});
        auto found = subscribers_.find(stream);
        if (found == subscribers_.end())
            return;
        for (auto &weak : found->second)
            if (auto subscription = weak.lock())
                subscription->offer(event);
        prune_locked(stream);
    }

    void EventStreamHub::close_all()
    {
        std::lock_guard lock(mutex_);
        for (auto &[_, list] : subscribers_)
            for (auto &weak : list)
                if (auto subscription = weak.lock())
                    subscription->close();
        subscribers_.clear();
    }
}
