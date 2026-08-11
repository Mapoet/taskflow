#pragma once

#include <agent/ui/store_backed_operations.hpp>

#include <memory>
#include <stdexcept>
#include <string>

namespace agent_framework::example {

struct Phase4OperationsBootstrapOptions {
    std::string database_path;
    std::string tenant_id{"demo-tenant"};
    std::string run_id{"run-orbit-042"};
    bool allow_demo_fallback{false};
    bool seed_persistent_if_missing{false};
};

struct Phase4OperationsBootstrap {
    std::shared_ptr<agent_framework::SQLiteOperationsSnapshotStore> store;
    agent_framework::Phase4OperationsSnapshot snapshot;
    bool persistent{false};
};

inline Phase4OperationsBootstrap load_phase4_operations(
    const Phase4OperationsBootstrapOptions& options) {
    Phase4OperationsBootstrap result;
    if (!options.database_path.empty()) {
        result.store = std::make_shared<agent_framework::SQLiteOperationsSnapshotStore>(
            options.database_path);
        auto stored = result.store->latest(options.tenant_id, options.run_id);
        if (!stored) {
            if (!options.seed_persistent_if_missing) {
                throw std::runtime_error("no Phase 4 operations snapshot for tenant=" +
                                         options.tenant_id + " run=" + options.run_id);
            }
            auto seeded = agent_framework::Phase4OperationsProjection::demo_snapshot();
            seeded.tenant_id = options.tenant_id;
            seeded.run_id = options.run_id;
            std::string error;
            if (!result.store->save(seeded, &error)) {
                throw std::runtime_error("cannot seed Phase 4 operations snapshot: " + error);
            }
            stored = std::move(seeded);
        }
        result.snapshot = std::move(*stored);
        result.persistent = true;
        return result;
    }
    if (!options.allow_demo_fallback) {
        throw std::invalid_argument("--operations-db is required for persistent operations state");
    }
    result.snapshot = agent_framework::Phase4OperationsProjection::demo_snapshot();
    return result;
}

}  // namespace agent_framework::example
