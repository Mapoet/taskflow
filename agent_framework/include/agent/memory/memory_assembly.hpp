#ifndef __AGENT_MEMORY_ASSEMBLY_H__
#define __AGENT_MEMORY_ASSEMBLY_H__

#include <agent/context_budget/context_budget.hpp>

#include <cstddef>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
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

/** The only typed boundary for prompt context assembly. */
struct MemoryAssemblyInput {
    std::vector<MemorySlot> system;
    std::vector<MemorySlot> task;
    std::vector<MemorySlot> working;
    std::vector<MemorySlot> retrieval;
    std::vector<MemorySlot> tool;
    std::vector<MemorySlot> skill;

    std::vector<MemorySlot> flatten() const;
};

struct MemoryAssemblyPolicy {
    std::size_t soft_limit_bytes = 0; // 0 means no soft cap
    std::size_t hard_limit_bytes = 0; // 0 means no hard cap
    std::size_t soft_limit_tokens = 0; // 0 means no soft cap
    std::size_t hard_limit_tokens = 0; // 0 means no hard cap
    std::size_t default_slot_quota_bytes = 0;
    std::map<MemorySlotKind, std::size_t> slot_quota_bytes;
    std::map<MemorySlotKind, std::size_t> minimum_retained_bytes;
    bool retain_system_and_task = true;
    bool retain_citations = true;
    bool fail_on_mandatory_overflow = true;
    std::string revision = "memory-assembly-v1";
    std::function<std::size_t(std::string_view)> token_estimator;
};

class MemoryAssemblyBudgetError : public std::length_error {
public:
    using std::length_error::length_error;
};

struct MemoryAssemblyReport {
    std::size_t input_bytes = 0;
    std::size_t output_bytes = 0;
    std::size_t input_tokens = 0;
    std::size_t output_tokens = 0;
    std::size_t input_slots = 0;
    std::size_t output_slots = 0;
    std::size_t evicted_slots = 0;
    std::string policy_revision;
    std::vector<json> decisions; // ids, sizes and reasons only; never slot text
    json to_json() const;
};

struct MemoryAssemblyResult {
    std::string text;
    std::vector<MemorySlot> slots;
    MemoryAssemblyReport report;
};

/** Deterministically applies quotas and priority eviction without exposing source text in reports. */
MemoryAssemblyResult assemble_memory(std::vector<MemorySlot> slots, const MemoryAssemblyPolicy& policy);
MemoryAssemblyResult assemble_memory(const MemoryAssemblyInput& input,
                                     const MemoryAssemblyPolicy& policy);
const char* memory_slot_kind_name(MemorySlotKind kind);

} // namespace agent_framework

#endif
