#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "agent/distributed/durable_queue.hpp"

namespace agent_framework::distributed {

// Shared, multi-host queue. Lease time is always evaluated by PostgreSQL;
// callers cannot influence ownership with a local clock.
class PostgresDurableQueue {
public:
    explicit PostgresDurableQueue(std::string conninfo);
    ~PostgresDurableQueue();
    PostgresDurableQueue(const PostgresDurableQueue&) = delete;
    PostgresDurableQueue& operator=(const PostgresDurableQueue&) = delete;

    bool enqueue(QueueTask task, std::string* error = nullptr);
    std::optional<Lease> claim(std::string_view worker_id,
                               std::string_view tenant_id,
                               std::int64_t lease_ms,
                               std::string* error = nullptr);
    std::optional<Lease> claim_with_quota(std::string_view worker_id,
                                          std::string_view tenant_id,
                                          std::int64_t lease_ms,
                                          std::string* error = nullptr);
    bool renew(std::string_view task_id, std::string_view worker_id,
               std::uint64_t fencing_token, std::int64_t lease_ms,
               std::string* error = nullptr);
    bool ack(std::string_view task_id, std::string_view worker_id,
             std::uint64_t fencing_token, std::string* error = nullptr);
    bool ack_with_quota(std::string_view task_id, std::string_view worker_id,
                        std::uint64_t fencing_token, std::string* error = nullptr);
    bool nack(std::string_view task_id, std::string_view worker_id,
              std::uint64_t fencing_token, std::int64_t delay_ms,
              std::string* error = nullptr);
    bool nack_with_quota(std::string_view task_id, std::string_view worker_id,
                         std::uint64_t fencing_token, std::int64_t delay_ms,
                         std::string* error = nullptr);
    bool set_quota(std::string_view tenant_id, std::uint64_t maximum_active,
                   std::string* error = nullptr);
    std::optional<TenantQuota> quota(std::string_view tenant_id,
                                     std::string* error = nullptr) const;
    std::optional<QueueTask> inspect(std::string_view task_id,
                                     std::string* error = nullptr) const;

private:
    void migrate();
    std::optional<Lease> claim_impl(std::string_view, std::string_view,
                                    std::int64_t, bool, std::string*);
    bool finish_impl(std::string_view, std::string_view, std::uint64_t,
                     bool, bool, std::int64_t, std::string*);
    void* connection_{nullptr};
    mutable std::mutex mutex_;
};

}  // namespace agent_framework::distributed
