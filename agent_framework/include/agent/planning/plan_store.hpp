#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "agent/planning/evidence_store.hpp"

namespace agent_framework::planning {

class PlanStore {
public:
    virtual ~PlanStore() = default;
    virtual PlanningCommitResult create(const ExecutionPlan& plan) = 0;
    virtual PlanningCommitResult compare_exchange(const ExecutionPlan& plan,
                                                   std::uint64_t expected_revision) = 0;
    virtual std::optional<ExecutionPlan> current(const contracts::ContractIdentity& identity) = 0;
    virtual std::vector<ExecutionPlan> history(const contracts::ContractIdentity& identity) = 0;
};

class InMemoryPlanStore final : public PlanStore {
public:
    PlanningCommitResult create(const ExecutionPlan& plan) override;
    PlanningCommitResult compare_exchange(const ExecutionPlan& plan,
                                           std::uint64_t expected_revision) override;
    std::optional<ExecutionPlan> current(const contracts::ContractIdentity& identity) override;
    std::vector<ExecutionPlan> history(const contracts::ContractIdentity& identity) override;

private:
    static std::string key(const contracts::ContractIdentity& identity);
    std::mutex mutex_;
    std::map<std::string, std::vector<ExecutionPlan>> plans_;
};

struct SQLitePlanningStoreOptions {
    int busy_timeout_ms{3000};
    bool require_private_permissions{true};
};

// A single database implements both stores so a cognition workflow can recover evidence and
// immutable plan revisions from one durable boundary.
class SQLitePlanningStore final : public EvidenceStore, public PlanStore {
public:
    explicit SQLitePlanningStore(std::string path, SQLitePlanningStoreOptions options = {});
    ~SQLitePlanningStore() override;
    SQLitePlanningStore(const SQLitePlanningStore&) = delete;
    SQLitePlanningStore& operator=(const SQLitePlanningStore&) = delete;

    PlanningCommitResult append(const contracts::ContractMetadata& scope,
                                const EvidenceRecord& record) override;
    std::optional<EvidenceRecord> get(const contracts::ContractMetadata& scope,
                                      std::string_view evidence_id) override;
    EvidenceBundle bundle(const contracts::ContractMetadata& scope,
                          const std::vector<std::string>& evidence_ids) override;
    PlanningCommitResult create(const ExecutionPlan& plan) override;
    PlanningCommitResult compare_exchange(const ExecutionPlan& plan,
                                           std::uint64_t expected_revision) override;
    std::optional<ExecutionPlan> current(const contracts::ContractIdentity& identity) override;
    std::vector<ExecutionPlan> history(const contracts::ContractIdentity& identity) override;

    const std::string& path() const noexcept { return path_; }

private:
    void migrate();
    void* db_{nullptr};
    std::string path_;
    SQLitePlanningStoreOptions options_;
    std::mutex mutex_;
};

}  // namespace agent_framework::planning
