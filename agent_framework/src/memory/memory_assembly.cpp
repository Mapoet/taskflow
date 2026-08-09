#include <agent/memory/memory_assembly.hpp>

#include <algorithm>

namespace agent_framework {
const char* memory_slot_kind_name(MemorySlotKind kind) {
    switch(kind) { case MemorySlotKind::System: return "system"; case MemorySlotKind::Task: return "task";
    case MemorySlotKind::Working: return "working"; case MemorySlotKind::Retrieval: return "retrieval";
    case MemorySlotKind::Tool: return "tool"; case MemorySlotKind::Skill: return "skill"; }
    return "working";
}
json MemoryAssemblyReport::to_json() const {
    return {{"input_bytes", input_bytes}, {"output_bytes", output_bytes},
            {"evicted_slots", evicted_slots}, {"decisions", decisions}};
}
MemoryAssemblyResult assemble_memory(std::vector<MemorySlot> slots, const MemoryAssemblyPolicy& policy) {
    MemoryAssemblyResult result;
    for(auto& slot : slots) {
        result.report.input_bytes += slot.text.size();
        const auto quota = slot.byte_quota ? slot.byte_quota : policy.default_slot_quota_bytes;
        if(quota && slot.text.size() > quota) {
            const auto before = slot.text.size(); slot.text = utf8_safe_truncate(slot.text, quota);
            result.report.decisions.push_back({{"source_id", slot.source_id}, {"kind", memory_slot_kind_name(slot.kind)},
                {"reason", "slot_quota"}, {"original_bytes", before}, {"kept_bytes", slot.text.size()}});
        }
    }
    std::stable_sort(slots.begin(), slots.end(), [](const auto& a, const auto& b) { return a.priority > b.priority; });
    std::size_t used = 0;
    for(auto& slot : slots) {
        const bool protected_slot = policy.retain_system_and_task &&
            (slot.kind == MemorySlotKind::System || slot.kind == MemorySlotKind::Task);
        const std::size_t cap = policy.hard_limit_bytes ? policy.hard_limit_bytes : policy.soft_limit_bytes;
        if(cap && used + slot.text.size() > cap && !protected_slot) {
            const auto left = used < cap ? cap - used : 0;
            const auto before = slot.text.size(); slot.text = utf8_safe_truncate(slot.text, left);
            result.report.evicted_slots += left == 0;
            result.report.decisions.push_back({{"source_id", slot.source_id}, {"kind", memory_slot_kind_name(slot.kind)},
                {"reason", left ? "budget_truncated" : "budget_evicted"}, {"original_bytes", before}, {"kept_bytes", slot.text.size()}});
        }
        if(slot.text.empty()) continue;
        if(!result.text.empty()) result.text += "\n\n";
        result.text += slot.text; used = result.text.size();
    }
    if(policy.hard_limit_bytes && result.text.size() > policy.hard_limit_bytes)
        result.text = utf8_safe_truncate(result.text, policy.hard_limit_bytes);
    result.report.output_bytes = result.text.size();
    return result;
}
} // namespace agent_framework
