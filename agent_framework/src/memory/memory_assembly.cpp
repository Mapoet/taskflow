#include <agent/memory/memory_assembly.hpp>
#include <agent/skills/skill_supply_chain.hpp>

#include <algorithm>
#include <limits>

namespace agent_framework {
namespace {
std::string source_digest(const std::string& source_id) {
    return skill_sha256_bytes(source_id).value_or("sha256-unavailable");
}
}

std::vector<MemorySlot> MemoryAssemblyInput::flatten() const {
    std::vector<MemorySlot> result;
    result.reserve(system.size() + task.size() + working.size() + retrieval.size() +
                   tool.size() + skill.size());
    const auto append = [&result](const std::vector<MemorySlot>& slots) {
        result.insert(result.end(), slots.begin(), slots.end());
    };
    append(system); append(task); append(working); append(retrieval); append(tool); append(skill);
    return result;
}

const char* memory_slot_kind_name(MemorySlotKind kind) {
    switch(kind) { case MemorySlotKind::System: return "system"; case MemorySlotKind::Task: return "task";
    case MemorySlotKind::Working: return "working"; case MemorySlotKind::Retrieval: return "retrieval";
    case MemorySlotKind::Tool: return "tool"; case MemorySlotKind::Skill: return "skill"; }
    return "working";
}
json MemoryAssemblyReport::to_json() const {
    return {{"input_bytes", input_bytes}, {"output_bytes", output_bytes},
            {"input_tokens", input_tokens}, {"output_tokens", output_tokens},
            {"input_slots", input_slots}, {"output_slots", output_slots},
            {"evicted_slots", evicted_slots}, {"policy_revision", policy_revision},
            {"decisions", decisions}};
}
MemoryAssemblyResult assemble_memory(std::vector<MemorySlot> slots, const MemoryAssemblyPolicy& policy) {
    MemoryAssemblyResult result;
    const auto estimate = policy.token_estimator ? policy.token_estimator :
        std::function<std::size_t(std::string_view)>([](std::string_view text) {
            return (text.size() + 3) / 4;
        });
    result.report.policy_revision = policy.revision;
    result.report.input_slots = slots.size();
    for(auto& slot : slots) {
        result.report.input_bytes += slot.text.size();
        result.report.input_tokens += estimate(slot.text);
        auto quota = slot.byte_quota;
        if(!quota) {
            const auto per_kind = policy.slot_quota_bytes.find(slot.kind);
            quota = per_kind == policy.slot_quota_bytes.end()
                ? policy.default_slot_quota_bytes : per_kind->second;
        }
        if(quota && slot.text.size() > quota) {
            const auto before = slot.text.size(); slot.text = utf8_safe_truncate(slot.text, quota);
            result.report.decisions.push_back({{"source_id_digest", source_digest(slot.source_id)}, {"kind", memory_slot_kind_name(slot.kind)},
                {"reason", "slot_quota"}, {"original_bytes", before}, {"kept_bytes", slot.text.size()}});
        }
    }
    const auto protected_slot = [&policy](const MemorySlot& slot) {
        return policy.retain_system_and_task &&
            (slot.kind == MemorySlotKind::System || slot.kind == MemorySlotKind::Task);
    };
    const auto kind_rank = [](MemorySlotKind kind) {
        switch(kind) {
        case MemorySlotKind::System: return 6;
        case MemorySlotKind::Task: return 5;
        case MemorySlotKind::Working: return 4;
        case MemorySlotKind::Retrieval: return 3;
        case MemorySlotKind::Skill: return 2;
        case MemorySlotKind::Tool: return 1;
        }
        return 0;
    };
    std::stable_sort(slots.begin(), slots.end(), [&](const auto& a, const auto& b) {
        if(protected_slot(a) != protected_slot(b)) return protected_slot(a);
        if(a.priority != b.priority) return a.priority > b.priority;
        return kind_rank(a.kind) > kind_rank(b.kind);
    });
    std::size_t used = 0;
    for(auto& slot : slots) {
        const bool is_protected = protected_slot(slot);
        const std::size_t hard = policy.hard_limit_bytes ? policy.hard_limit_bytes
            : policy.soft_limit_bytes ? policy.soft_limit_bytes : std::numeric_limits<std::size_t>::max();
        const std::size_t soft = policy.soft_limit_bytes
            ? std::min(policy.soft_limit_bytes, hard) : hard;
        const std::size_t cap = is_protected ? hard : soft;
        const std::size_t separator = result.text.empty() ? 0 : 2;
        if(used + separator + slot.text.size() > cap) {
            const auto available = used + separator < cap ? cap - used - separator : 0;
            const auto minimum_it = policy.minimum_retained_bytes.find(slot.kind);
            const auto minimum = minimum_it == policy.minimum_retained_bytes.end() ? 0 : minimum_it->second;
            const auto before = slot.text.size();
            if(available < minimum) slot.text.clear();
            else slot.text = utf8_safe_truncate(slot.text, available);
            if(slot.text.empty()) ++result.report.evicted_slots;
            result.report.decisions.push_back({
                {"source_id_digest", source_digest(slot.source_id)},
                {"kind", memory_slot_kind_name(slot.kind)},
                {"reason", slot.text.empty() ? "budget_evicted" : "budget_truncated"},
                {"original_bytes", before}, {"kept_bytes", slot.text.size()},
                {"citation_retained", policy.retain_citations && slot.provenance.contains("citation_id") && !slot.text.empty()}});
        }
        if(slot.text.empty()) continue;
        if(!result.text.empty()) result.text += "\n\n";
        result.text += slot.text;
        used = result.text.size();
        result.slots.push_back(slot);
    }
    result.report.output_bytes = result.text.size();
    result.report.output_tokens = estimate(result.text);
    result.report.output_slots = result.slots.size();
    return result;
}

MemoryAssemblyResult assemble_memory(const MemoryAssemblyInput& input,
                                     const MemoryAssemblyPolicy& policy) {
    return assemble_memory(input.flatten(), policy);
}
} // namespace agent_framework
