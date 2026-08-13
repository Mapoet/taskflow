#include "agent/tool_runtime/event_stream.hpp"
#include <algorithm>

namespace agent_framework::tool_runtime
{
struct InvocationEventSubscription::PullState
{
    explicit PullState(InvocationStore &value):store(&value){}
    std::mutex mutex;InvocationStore *store;
};

InvocationEventSubscription::InvocationEventSubscription(std::string id,std::uint64_t cursor,
    std::size_t capacity,std::shared_ptr<PullState> state,std::chrono::milliseconds poll)
    :invocation_id_(std::move(id)),cursor_(cursor),capacity_(std::max<std::size_t>(1,capacity)),
     pull_state_(std::move(state)),poll_interval_(poll){}

void InvocationEventSubscription::offer(const InvocationEvent &event)
{
    std::lock_guard lock(mutex_);if(closed_||event.sequence<=cursor_)return;
    auto pos=std::lower_bound(pending_.begin(),pending_.end(),event.sequence,
        [](const InvocationEvent&e,std::uint64_t seq){return e.sequence<seq;});
    if(pos!=pending_.end()&&pos->sequence==event.sequence)return;
    if(pending_.size()>=capacity_){overflowed_=closed_=true;ready_.notify_all();return;}
    pending_.insert(pos,event);ready_.notify_one();
}

InvocationSubscriptionRead InvocationEventSubscription::next(InvocationEvent &event,
    std::chrono::milliseconds timeout)
{
    const auto deadline=std::chrono::steady_clock::now()+std::max(timeout,std::chrono::milliseconds(0));
    for(;;){
        {std::unique_lock lock(mutex_);if(!pending_.empty()){event=std::move(pending_.front());pending_.pop_front();cursor_=std::max(cursor_,event.sequence);return InvocationSubscriptionRead::Event;}if(closed_)return overflowed_?InvocationSubscriptionRead::Overflow:terminal_;}
        auto pull=pull_from_store();if(pull==InvocationSubscriptionRead::CursorExpired||pull==InvocationSubscriptionRead::IntegrityFailure||pull==InvocationSubscriptionRead::Overflow)return pull;
        auto now=std::chrono::steady_clock::now();if(now>=deadline)return InvocationSubscriptionRead::Timeout;
        std::unique_lock lock(mutex_);ready_.wait_for(lock,std::min(poll_interval_,std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now)),[&]{return closed_||!pending_.empty();});
    }
}

InvocationSubscriptionRead InvocationEventSubscription::pull_from_store()
{
    std::uint64_t cursor;std::size_t available;{std::lock_guard lock(mutex_);if(closed_)return overflowed_?InvocationSubscriptionRead::Overflow:terminal_;cursor=cursor_;available=capacity_-pending_.size();if(!available)return InvocationSubscriptionRead::Timeout;}
    std::unique_lock state_lock(pull_state_->mutex);auto *store=pull_state_->store;if(!store)return InvocationSubscriptionRead::Closed;
    auto floor=store->event_retention_floor(invocation_id_),head=store->event_head(invocation_id_);
    if(floor&&cursor+1<floor){state_lock.unlock();std::lock_guard lock(mutex_);terminal_=InvocationSubscriptionRead::CursorExpired;closed_=true;ready_.notify_all();return terminal_;}
    if(head<=cursor)return InvocationSubscriptionRead::Timeout;
    auto replay=store->events(invocation_id_,cursor,available+1);state_lock.unlock();
    if(replay.empty()||replay.front().sequence!=cursor+1){std::lock_guard lock(mutex_);terminal_=InvocationSubscriptionRead::IntegrityFailure;closed_=true;ready_.notify_all();return terminal_;}
    if(replay.size()>available){std::lock_guard lock(mutex_);overflowed_=closed_=true;ready_.notify_all();return InvocationSubscriptionRead::Overflow;}
    auto expected=cursor+1;for(const auto&e:replay)if(e.sequence!=expected++){std::lock_guard lock(mutex_);terminal_=InvocationSubscriptionRead::IntegrityFailure;closed_=true;ready_.notify_all();return terminal_;}
    for(const auto&e:replay) offer(e);
    return InvocationSubscriptionRead::Event;
}

void InvocationEventSubscription::close(){std::lock_guard lock(mutex_);closed_=true;ready_.notify_all();}
std::uint64_t InvocationEventSubscription::cursor()const{std::lock_guard lock(mutex_);return cursor_;}
bool InvocationEventSubscription::overflowed()const{std::lock_guard lock(mutex_);return overflowed_;}

InvocationEventStreamHub::InvocationEventStreamHub(InvocationStore&s):store_(s),pull_state_(std::make_shared<InvocationEventSubscription::PullState>(s)){}
InvocationEventStreamHub::~InvocationEventStreamHub(){close_all();}
InvocationSubscribeResult InvocationEventStreamHub::subscribe(std::string_view id,std::uint64_t after,std::size_t capacity)
{
    std::lock_guard lock(mutex_);auto head=store_.event_head(id),floor=store_.event_retention_floor(id);
    if(after>head)return {{},head,"cursor_ahead_of_head"};
    if(floor&&after+1<floor)return {{},head,"cursor_expired"};
    auto replay=store_.events(id,after,std::max<std::size_t>(1,capacity)+1);if(head>after&&replay.empty())return {{},head,"event_replay_integrity_failure"};if(replay.size()>std::max<std::size_t>(1,capacity))return {{},head,"replay_exceeds_capacity"};
    auto sub=std::shared_ptr<InvocationEventSubscription>(new InvocationEventSubscription(std::string(id),after,capacity,pull_state_,std::chrono::milliseconds(100)));for(const auto&e:replay)sub->offer(e);subscribers_[std::string(id)].push_back(sub);return {std::move(sub),head,{}};
}
void InvocationEventStreamHub::prune_locked(const std::string&id){auto it=subscribers_.find(id);if(it==subscribers_.end())return;auto&v=it->second;v.erase(std::remove_if(v.begin(),v.end(),[](auto&w){auto s=w.lock();return !s||s->overflowed();}),v.end());if(v.empty())subscribers_.erase(it);}
void InvocationEventStreamHub::publish(const InvocationEvent&e){if(e.durability!=InvocationEventDurability::Durable||!e.sequence)return;std::lock_guard lock(mutex_);auto it=subscribers_.find(e.invocation_id);if(it==subscribers_.end())return;for(auto&w:it->second)if(auto s=w.lock())s->offer(e);prune_locked(e.invocation_id);}
void InvocationEventStreamHub::close_all(){{std::lock_guard lock(pull_state_->mutex);pull_state_->store=nullptr;}std::lock_guard lock(mutex_);for(auto&[_,v]:subscribers_)for(auto&w:v)if(auto s=w.lock())s->close();subscribers_.clear();}
}
