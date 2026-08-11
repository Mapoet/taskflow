#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

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

class SQLiteDurableQueue {
public:
    explicit SQLiteDurableQueue(std::string path, int busy_timeout_ms = 3000);
    ~SQLiteDurableQueue();
    SQLiteDurableQueue(const SQLiteDurableQueue&) = delete;
    SQLiteDurableQueue& operator=(const SQLiteDurableQueue&) = delete;
    bool enqueue(QueueTask task, std::string* error = nullptr);
    std::optional<Lease> claim(std::string_view worker_id, std::string_view tenant_id,
                               std::int64_t now_ms, std::int64_t lease_ms);
    // Production scheduler path: queue lease and tenant quota are changed in one
    // database transaction. Expired lease takeover retains the existing quota slot.
    std::optional<Lease> claim_with_quota(std::string_view worker_id,
                                          std::string_view tenant_id,
                                          std::int64_t now_ms,
                                          std::int64_t lease_ms);
    bool renew(std::string_view task_id, std::string_view worker_id,
               std::uint64_t fencing_token, std::int64_t now_ms, std::int64_t lease_ms);
    bool ack(std::string_view task_id, std::string_view worker_id,
             std::uint64_t fencing_token);
    bool ack_with_quota(std::string_view task_id, std::string_view worker_id,
                        std::uint64_t fencing_token);
    bool ack_with_quota_at(std::string_view task_id, std::string_view worker_id,
                           std::uint64_t fencing_token, std::int64_t now_ms);
    bool nack(std::string_view task_id, std::string_view worker_id,
              std::uint64_t fencing_token, std::int64_t available_at_ms);
    bool nack_with_quota(std::string_view task_id, std::string_view worker_id,
                         std::uint64_t fencing_token, std::int64_t available_at_ms);
    bool nack_with_quota_at(std::string_view task_id, std::string_view worker_id,
                            std::uint64_t fencing_token, std::int64_t now_ms,
                            std::int64_t available_at_ms);
    std::optional<QueueTask> inspect(std::string_view task_id) const;
private:
    void migrate();
    void* db_{nullptr};
    mutable std::mutex mutex_;
};

struct WorkerRecord {
    std::string worker_id;
    std::string instance_id;
    std::string capabilities_digest;
    std::uint64_t generation{0};
    std::int64_t heartbeat_at_ms{0};
};
struct TenantQuota {
    std::string tenant_id;
    std::uint64_t maximum_active{0};
    std::uint64_t active{0};
};
class SQLiteWorkerRegistry {
public:
    explicit SQLiteWorkerRegistry(std::string path, int busy_timeout_ms = 3000);
    ~SQLiteWorkerRegistry();
    SQLiteWorkerRegistry(const SQLiteWorkerRegistry&) = delete;
    SQLiteWorkerRegistry& operator=(const SQLiteWorkerRegistry&) = delete;
    std::optional<std::uint64_t> register_worker(const WorkerRecord& value,
                                                 std::string* error = nullptr);
    bool heartbeat(std::string_view worker_id, std::string_view instance_id,
                   std::uint64_t generation, std::int64_t now_ms);
    std::vector<WorkerRecord> alive(std::int64_t not_before_ms) const;
    bool set_quota(std::string_view tenant_id, std::uint64_t maximum_active);
    bool reserve(std::string_view tenant_id);
    bool release(std::string_view tenant_id);
    std::optional<TenantQuota> quota(std::string_view tenant_id) const;
private:
    void migrate();
    void* db_{nullptr};
    mutable std::mutex mutex_;
};

}  // namespace agent_framework::distributed
