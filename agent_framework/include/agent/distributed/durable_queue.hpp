#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace agent_framework::distributed {

enum class QueueState { Pending, Leased, Completed, DeadLetter };
struct QueueTask {
    std::string task_id;
    std::string tenant_id;
    std::string idempotency_key;
    std::string payload_digest;
    int priority{0};
    std::int64_t available_at_ms{0};
    std::uint32_t attempts{0};
    std::uint32_t max_attempts{3};
    QueueState state{QueueState::Pending};
    std::string owner;
    std::uint64_t fencing_token{0};
    std::int64_t lease_expires_at_ms{0};
};
struct Lease {
    QueueTask task;
    std::uint64_t fencing_token{0};
};

class InMemoryDurableQueue {
public:
    bool enqueue(QueueTask task, std::string* error = nullptr);
    std::optional<Lease> claim(std::string_view worker_id, std::string_view tenant_id,
                               std::int64_t now_ms, std::int64_t lease_ms);
    bool renew(std::string_view task_id, std::string_view worker_id, std::uint64_t fencing_token,
               std::int64_t now_ms, std::int64_t lease_ms);
    bool ack(std::string_view task_id, std::string_view worker_id, std::uint64_t fencing_token);
    bool nack(std::string_view task_id, std::string_view worker_id, std::uint64_t fencing_token,
              std::int64_t available_at_ms);
    std::optional<QueueTask> inspect(std::string_view task_id) const;
private:
    bool owns(const QueueTask& task, std::string_view worker, std::uint64_t token) const;
    mutable std::mutex mutex_;
    std::map<std::string, QueueTask> tasks_;
    std::map<std::string, std::string> idempotency_;
};

}  // namespace agent_framework::distributed
