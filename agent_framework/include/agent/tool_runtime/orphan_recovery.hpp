#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "agent/tool_runtime/store.hpp"

namespace agent_framework::tool_runtime {

struct OrphanRecoveryRecord {
    std::string invocation_id;
    InvocationState prior_state{InvocationState::Created};
    InvocationState recovery_state{InvocationState::Created};
    std::uint64_t fencing_token{0};
    std::string action;
    std::string error;
};

struct OrphanRecoveryReport {
    std::size_t inspected{0};
    std::size_t orphaned{0};
    std::size_t queued_for_reconcile{0};
    std::size_t failures{0};
    std::vector<OrphanRecoveryRecord> records;
};

class InvocationOrphanSweeper {
public:
    explicit InvocationOrphanSweeper(InvocationStore& store) : store_(store) {}
    OrphanRecoveryReport sweep(std::int64_t now_ms, std::size_t limit = 100);

private:
    InvocationStore& store_;
};

}  // namespace agent_framework::tool_runtime
