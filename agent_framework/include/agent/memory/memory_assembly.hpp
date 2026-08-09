#ifndef __AGENT_MEMORY_ASSEMBLY_H__
#define __AGENT_MEMORY_ASSEMBLY_H__

#include <agent/context_budget/context_budget.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace agent_framework {

enum class MemorySlotKind { System, Task, Working, Retrieval, Tool, Skill };

struct MemorySlot {
    MemorySlotKind kind = MemorySlotKind::Working;
    std::string source_id;
    int priority = 0;
    std::size_t byte_quota = 0;  // 0 means policy default
    std::string text;
    json provenance = json::object();
};

struct MemoryAssemblyPolicy {
    std::size_t soft_limit_bytes = 0; // 0 means no soft cap
    std::size_t hard_limit_bytes = 0; // 0 means no hard cap
    std::size_t default_slot_quota_bytes = 0;
    bool retain_system_and_task = true;
};

struct MemoryAssemblyReport {
    std::size_t input_bytes = 0;
    std::size_t output_bytes = 0;
    std::size_t evicted_slots = 0;
    std::vector<json> decisions; // ids, sizes and reasons only; never slot text
    json to_json() const;
};

struct MemoryAssemblyResult {
    std::string text;
    MemoryAssemblyReport report;
};

/** Deterministically applies quotas and priority eviction without exposing source text in reports. */
MemoryAssemblyResult assemble_memory(std::vector<MemorySlot> slots, const MemoryAssemblyPolicy& policy);
const char* memory_slot_kind_name(MemorySlotKind kind);

} // namespace agent_framework

#endif
