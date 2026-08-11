#pragma once

#include <mutex>
#include <optional>
#include <string>

#include "agent/approval/store.hpp"
#include "agent/harness/store.hpp"
#include "agent/memory_v2/store.hpp"
#include "agent/ui/phase4_operations.hpp"

namespace agent_framework {

class SQLiteOperationsSnapshotStore {
public:
    explicit SQLiteOperationsSnapshotStore(std::string path);
    ~SQLiteOperationsSnapshotStore();
    SQLiteOperationsSnapshotStore(const SQLiteOperationsSnapshotStore&) = delete;
    SQLiteOperationsSnapshotStore& operator=(const SQLiteOperationsSnapshotStore&) = delete;
    bool save(const Phase4OperationsSnapshot& snapshot, std::string* error = nullptr);
    std::optional<Phase4OperationsSnapshot> load(std::string_view snapshot_id);
    std::optional<Phase4OperationsSnapshot> latest(std::string_view tenant_id,
                                                    std::string_view run_id);
private:
    void* db_{nullptr};
    std::mutex mutex_;
};

struct OperationsAssemblyRequest {
    std::string tenant_id;
    std::string harness_id;
    std::string principal_id;
    std::string now;
    memory_v2::MemoryScope memory_subject;
};

class StoreBackedOperationsAssembler {
public:
    StoreBackedOperationsAssembler(harness::HarnessStore& harnesses,
                                   approval::ApprovalStore& approvals,
                                   memory_v2::MemoryStore& memories,
                                   SQLiteOperationsSnapshotStore& snapshots);
    std::optional<Phase4OperationsSnapshot> assemble(
        const OperationsAssemblyRequest& request, std::string* error = nullptr);
private:
    harness::HarnessStore& harnesses_;
    approval::ApprovalStore& approvals_;
    memory_v2::MemoryStore& memories_;
    SQLiteOperationsSnapshotStore& snapshots_;
};

}  // namespace agent_framework
