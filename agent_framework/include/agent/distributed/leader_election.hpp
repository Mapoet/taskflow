#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace agent_framework::distributed {

struct LeaderLease {
    std::string election;
    std::string owner;
    std::uint64_t fencing_token{0};
    std::int64_t expires_at_ms{0};
};

class SQLiteLeaderElection {
public:
    explicit SQLiteLeaderElection(std::string path, int busy_timeout_ms = 3000);
    ~SQLiteLeaderElection();
    SQLiteLeaderElection(const SQLiteLeaderElection&) = delete;
    SQLiteLeaderElection& operator=(const SQLiteLeaderElection&) = delete;
    std::optional<LeaderLease> acquire(std::string_view election,
                                       std::string_view owner,
                                       std::int64_t now_ms,
                                       std::int64_t lease_ms);
    bool renew(const LeaderLease& lease, std::int64_t now_ms,
               std::int64_t lease_ms);
    bool release(const LeaderLease& lease);
    bool is_current(const LeaderLease& lease, std::int64_t now_ms) const;
    std::optional<LeaderLease> inspect(std::string_view election) const;
private:
    void migrate();
    void* db_{nullptr};
    mutable std::mutex mutex_;
};

}  // namespace agent_framework::distributed
