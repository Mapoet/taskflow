#include "agent/distributed/durable_queue.hpp"

#include <algorithm>
#include <vector>

namespace agent_framework::distributed {

bool InMemoryDurableQueue::enqueue(QueueTask task, std::string* error) {
    if(task.task_id.empty() || task.tenant_id.empty() || task.idempotency_key.empty() ||
       task.payload_digest.empty() || task.max_attempts == 0) {
        if(error) *error = "queue task identity, payload, and attempts are required";
        return false;
    }
    std::lock_guard lock(mutex_);
    const auto idempotency = task.tenant_id + '\x1f' + task.idempotency_key;
    if(idempotency_.count(idempotency) || tasks_.count(task.task_id)) {
        if(error) *error = "duplicate queue task";
        return false;
    }
    idempotency_[idempotency] = task.task_id;
    tasks_.emplace(task.task_id, std::move(task));
    return true;
}

std::optional<Lease> InMemoryDurableQueue::claim(
    std::string_view worker, std::string_view tenant, std::int64_t now, std::int64_t lease_ms) {
    if(worker.empty() || tenant.empty() || lease_ms <= 0) return std::nullopt;
    std::lock_guard lock(mutex_);
    std::vector<QueueTask*> eligible;
    for(auto& [id, task] : tasks_) {
        (void)id;
        if(task.tenant_id != tenant || task.state == QueueState::Completed ||
           task.state == QueueState::DeadLetter || task.available_at_ms > now) continue;
        if(task.state == QueueState::Leased && task.lease_expires_at_ms > now) continue;
        if(task.attempts >= task.max_attempts) {
            task.state = QueueState::DeadLetter;
            task.owner.clear();
            task.lease_expires_at_ms = 0;
            continue;
        }
        eligible.push_back(&task);
    }
    if(eligible.empty()) return std::nullopt;
    std::sort(eligible.begin(), eligible.end(), [](const auto* a, const auto* b) {
        return a->priority != b->priority ? a->priority > b->priority : a->task_id < b->task_id;
    });
    auto& task = *eligible.front();
    task.state = QueueState::Leased;
    task.owner = std::string(worker);
    ++task.fencing_token;
    ++task.attempts;
    task.lease_expires_at_ms = now + lease_ms;
    return Lease{task, task.fencing_token};
}

bool InMemoryDurableQueue::owns(
    const QueueTask& task, std::string_view worker, std::uint64_t token) const {
    return task.state == QueueState::Leased && task.owner == worker && task.fencing_token == token;
}

bool InMemoryDurableQueue::renew(
    std::string_view id, std::string_view worker, std::uint64_t token,
    std::int64_t now, std::int64_t lease_ms) {
    std::lock_guard lock(mutex_);
    auto found = tasks_.find(std::string(id));
    if(found == tasks_.end() || !owns(found->second, worker, token) ||
       found->second.lease_expires_at_ms <= now || lease_ms <= 0) return false;
    found->second.lease_expires_at_ms = now + lease_ms;
    return true;
}

bool InMemoryDurableQueue::ack(
    std::string_view id, std::string_view worker, std::uint64_t token) {
    std::lock_guard lock(mutex_);
    auto found = tasks_.find(std::string(id));
    if(found == tasks_.end() || !owns(found->second, worker, token)) return false;
    found->second.state = QueueState::Completed;
    found->second.owner.clear();
    return true;
}

bool InMemoryDurableQueue::nack(
    std::string_view id, std::string_view worker, std::uint64_t token,
    std::int64_t available) {
    std::lock_guard lock(mutex_);
    auto found = tasks_.find(std::string(id));
    if(found == tasks_.end() || !owns(found->second, worker, token)) return false;
    found->second.state = found->second.attempts >= found->second.max_attempts
        ? QueueState::DeadLetter : QueueState::Pending;
    found->second.owner.clear();
    found->second.lease_expires_at_ms = 0;
    found->second.available_at_ms = available;
    return true;
}

std::optional<QueueTask> InMemoryDurableQueue::inspect(std::string_view id) const {
    std::lock_guard lock(mutex_);
    const auto found = tasks_.find(std::string(id));
    return found == tasks_.end() ? std::nullopt : std::optional<QueueTask>(found->second);
}

}  // namespace agent_framework::distributed
