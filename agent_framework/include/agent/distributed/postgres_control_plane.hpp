#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/distributed/durable_queue.hpp"
#include "agent/distributed/leader_election.hpp"

namespace agent_framework::distributed {

class PostgresWorkerRegistry {
public:
    explicit PostgresWorkerRegistry(std::string conninfo);
    ~PostgresWorkerRegistry();
    PostgresWorkerRegistry(const PostgresWorkerRegistry&) = delete;
    PostgresWorkerRegistry& operator=(const PostgresWorkerRegistry&) = delete;
    std::optional<std::uint64_t> register_worker(std::string_view worker_id,
        std::string_view instance_id, std::string_view capabilities_digest,
        std::string* error = nullptr);
    bool heartbeat(std::string_view worker_id, std::string_view instance_id,
                   std::uint64_t generation, std::string* error = nullptr);
    std::vector<WorkerRecord> alive(std::int64_t maximum_staleness_ms,
                                    std::string* error = nullptr) const;
private:
    void migrate();
    void* connection_{nullptr};
    mutable std::mutex mutex_;
};

class PostgresLeaderElection {
public:
    explicit PostgresLeaderElection(std::string conninfo);
    ~PostgresLeaderElection();
    PostgresLeaderElection(const PostgresLeaderElection&) = delete;
    PostgresLeaderElection& operator=(const PostgresLeaderElection&) = delete;
    std::optional<LeaderLease> acquire(std::string_view election,
        std::string_view owner, std::int64_t lease_ms, std::string* error = nullptr);
    bool renew(const LeaderLease& lease, std::int64_t lease_ms,
               std::string* error = nullptr);
    bool release(const LeaderLease& lease, std::string* error = nullptr);
    bool is_current(const LeaderLease& lease, std::string* error = nullptr) const;
    std::optional<LeaderLease> inspect(std::string_view election,
                                       std::string* error = nullptr) const;
private:
    void migrate();
    void* connection_{nullptr};
    mutable std::mutex mutex_;
};

}  // namespace agent_framework::distributed
