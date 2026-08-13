#include "agent/conversation/event_stream.hpp"
#include <algorithm>

namespace agent_framework::conversation
{
    struct EventSubscription::PullState
    {
        explicit PullState(ConversationStore &value) : store(&value) {}
        std::mutex mutex;
        ConversationStore *store;
    };

    EventSubscription::EventSubscription(ConversationIdentity identity, std::uint64_t cursor,
                                         std::size_t capacity, std::shared_ptr<PullState> pull_state,
                                         std::chrono::milliseconds poll_interval)
        : identity_(std::move(identity)), cursor_(cursor), capacity_(std::max<std::size_t>(1, capacity)),
          pull_state_(std::move(pull_state)), poll_interval_(poll_interval) {}

    void EventSubscription::offer(const RuntimeEventEnvelope &event)
    {
        std::lock_guard lock(mutex_);
        if (closed_ || event.sequence <= cursor_)
            return;
        auto position = std::lower_bound(pending_.begin(), pending_.end(), event.sequence,
            [](const RuntimeEventEnvelope &value, std::uint64_t sequence) {
                return value.sequence < sequence;
            });
        if (position != pending_.end() && position->sequence == event.sequence)
            return;
        if (pending_.size() >= capacity_)
        {
            overflowed_ = true;
            closed_ = true;
            ready_.notify_all();
            return;
        }
        pending_.insert(position, event);
        ready_.notify_one();
    }

    SubscriptionRead EventSubscription::next(RuntimeEventEnvelope &event,
                                               std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds(0));
        for (;;)
        {
            {
                std::unique_lock lock(mutex_);
                if (!pending_.empty())
                {
                    event = std::move(pending_.front());
                    pending_.pop_front();
                    cursor_ = std::max(cursor_, event.sequence);
                    return SubscriptionRead::Event;
                }
                if (closed_)
                    return overflowed_ ? SubscriptionRead::Overflow : terminal_;
            }
            const auto pull = pull_from_store();
            if (pull == SubscriptionRead::CursorExpired ||
                pull == SubscriptionRead::IntegrityFailure || pull == SubscriptionRead::Overflow)
                return pull;
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return SubscriptionRead::Timeout;
            std::unique_lock lock(mutex_);
            ready_.wait_for(lock, std::min(poll_interval_,
                                           std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)),
                            [&] { return closed_ || !pending_.empty(); });
        }
    }

    SubscriptionRead EventSubscription::pull_from_store()
    {
        std::uint64_t cursor;
        std::size_t available;
        {
            std::lock_guard lock(mutex_);
            if (closed_) return overflowed_ ? SubscriptionRead::Overflow : terminal_;
            cursor = cursor_;
            available = capacity_ - pending_.size();
            if (available == 0) return SubscriptionRead::Timeout;
        }
        ConversationStore *store = nullptr;
        std::unique_lock state_lock(pull_state_->mutex);
        store = pull_state_->store;
        if (!store) return SubscriptionRead::Closed;
        const auto floor = store->event_retention_floor(identity_);
        const auto head = store->last_event_sequence(identity_);
        if (floor > 0 && cursor + 1 < floor)
        {
            state_lock.unlock();
            std::lock_guard lock(mutex_); terminal_ = SubscriptionRead::CursorExpired; closed_ = true;
            ready_.notify_all(); return terminal_;
        }
        if (head <= cursor) return SubscriptionRead::Timeout;
        auto replay = store->events(identity_, cursor, available + 1);
        state_lock.unlock();
        if (replay.empty() || replay.front().sequence != cursor + 1)
        {
            std::lock_guard lock(mutex_); terminal_ = SubscriptionRead::IntegrityFailure; closed_ = true;
            ready_.notify_all(); return terminal_;
        }
        if (replay.size() > available)
        {
            std::lock_guard lock(mutex_); overflowed_ = true; closed_ = true;
            ready_.notify_all(); return SubscriptionRead::Overflow;
        }
        std::uint64_t expected = cursor + 1;
        for (const auto &value : replay)
            if (value.sequence != expected++)
            {
                std::lock_guard lock(mutex_); terminal_ = SubscriptionRead::IntegrityFailure; closed_ = true;
                ready_.notify_all(); return terminal_;
            }
        for (const auto &value : replay) offer(value);
        return SubscriptionRead::Event;
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

    EventStreamHub::EventStreamHub(ConversationStore &store)
        : store_(store), pull_state_(std::make_shared<EventSubscription::PullState>(store)) {}

    EventStreamHub::~EventStreamHub() { close_all(); }

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
            new EventSubscription(identity, after, capacity, pull_state_, std::chrono::milliseconds(100)));
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
        {
            std::lock_guard state_lock(pull_state_->mutex);
            pull_state_->store = nullptr;
        }
        std::lock_guard lock(mutex_);
        for (auto &[_, list] : subscribers_)
            for (auto &weak : list)
                if (auto subscription = weak.lock()) subscription->close();
        subscribers_.clear();
    }
}
