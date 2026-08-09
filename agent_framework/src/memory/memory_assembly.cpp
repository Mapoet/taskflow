#include <agent/memory/memory_assembly.hpp>
#include <agent/skills/skill_supply_chain.hpp>

#include <algorithm>
#include <cctype>
#include <limits>

namespace agent_framework {
namespace {
std::string source_digest(const std::string& source_id) {
    return skill_sha256_bytes(source_id).value_or("sha256-unavailable");
}

std::string citation_marker(const MemorySlot& slot) {
    if(!slot.provenance.contains("citation_id") ||
       !slot.provenance["citation_id"].is_string()) return {};
    return "[source:" + slot.provenance["citation_id"].get<std::string>() + "]";
}

std::string fit_slot_text(const MemorySlot& slot, std::size_t maximum_bytes,
                          bool retain_citations) {
    if(maximum_bytes >= slot.text.size()) return slot.text;
    const auto marker = retain_citations ? citation_marker(slot) : std::string{};
    if(marker.empty()) return utf8_safe_truncate(slot.text, maximum_bytes);
    if(maximum_bytes < marker.size()) return {};
    std::string body = slot.text;
    if(const auto position = body.find(marker); position != std::string::npos)
        body.erase(position, marker.size());
    while(!body.empty() && std::isspace(static_cast<unsigned char>(body.front())))
        body.erase(body.begin());
    if(body.empty() || maximum_bytes == marker.size()) return marker;
    const auto body_budget = maximum_bytes - marker.size() - 1;
    return marker + " " + utf8_safe_truncate(body, body_budget);
}

std::string fit_slot_to_caps(const MemorySlot& slot, std::size_t maximum_bytes,
                             std::string_view prefix, std::size_t maximum_tokens,
                             const std::function<std::size_t(std::string_view)>& estimate,
                             bool retain_citations) {
    const auto fits_tokens = [&](const std::string& value) {
        if(maximum_tokens == std::numeric_limits<std::size_t>::max()) return true;
        std::string combined(prefix);
        if(!combined.empty() && !value.empty()) combined += "\n\n";
        combined += value;
        return estimate(combined) <= maximum_tokens;
    };
    auto candidate = fit_slot_text(slot, maximum_bytes, retain_citations);
    if(fits_tokens(candidate)) return candidate;
    std::size_t low = 0, high = std::min(maximum_bytes, slot.text.size());
    std::string best;
    while(low <= high) {
        const auto middle = low + (high - low) / 2;
        auto value = fit_slot_text(slot, middle, retain_citations);
        if(fits_tokens(value)) {
            best = std::move(value);
            low = middle + 1;
        } else {
            if(middle == 0) break;
            high = middle - 1;
        }
    }
    return best;
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
        const bool mandatory = policy.retain_system_and_task &&
            (slot.kind == MemorySlotKind::System || slot.kind == MemorySlotKind::Task);
        if(quota && slot.text.size() > quota && !mandatory) {
            const auto before = slot.text.size();
            slot.text = fit_slot_text(slot, quota, policy.retain_citations);
            result.report.decisions.push_back({{"source_id_digest", source_digest(slot.source_id)}, {"kind", memory_slot_kind_name(slot.kind)},
                {"reason", "slot_quota"}, {"original_bytes", before}, {"kept_bytes", slot.text.size()},
                {"citation_retained", citation_marker(slot).empty() || slot.text.find(citation_marker(slot)) != std::string::npos}});
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

    std::string mandatory_text;
    for(const auto& slot : slots) {
        if(!protected_slot(slot)) continue;
        if(!mandatory_text.empty()) mandatory_text += "\n\n";
        mandatory_text += slot.text;
    }
    if(policy.fail_on_mandatory_overflow &&
       ((policy.hard_limit_bytes && mandatory_text.size() > policy.hard_limit_bytes) ||
        (policy.hard_limit_tokens && estimate(mandatory_text) > policy.hard_limit_tokens)))
        throw MemoryAssemblyBudgetError("mandatory system/task memory exceeds hard budget");

    std::size_t used = 0;
    for(auto& slot : slots) {
        const bool is_protected = protected_slot(slot);
        const std::size_t hard = policy.hard_limit_bytes ? policy.hard_limit_bytes
            : std::numeric_limits<std::size_t>::max();
        const std::size_t soft = policy.soft_limit_bytes
            ? std::min(policy.soft_limit_bytes, hard) : hard;
        const std::size_t cap = is_protected ? hard : soft;
        const std::size_t hard_tokens = policy.hard_limit_tokens ? policy.hard_limit_tokens
            : std::numeric_limits<std::size_t>::max();
        const std::size_t soft_tokens = policy.soft_limit_tokens
            ? std::min(policy.soft_limit_tokens, hard_tokens) : hard_tokens;
        const std::size_t token_cap = is_protected ? hard_tokens : soft_tokens;
        const std::size_t separator = result.text.empty() ? 0 : 2;
        const auto available = cap == std::numeric_limits<std::size_t>::max() ? cap
            : used + separator < cap ? cap - used - separator : 0;
        auto fitted = fit_slot_to_caps(slot, available, result.text, token_cap,
                                       estimate, policy.retain_citations);
        if(fitted != slot.text) {
            const auto minimum_it = policy.minimum_retained_bytes.find(slot.kind);
            const auto minimum = minimum_it == policy.minimum_retained_bytes.end() ? 0 : minimum_it->second;
            const auto before = slot.text.size();
            if(fitted.size() < minimum) fitted.clear();
            slot.text = std::move(fitted);
            if(slot.text.empty()) ++result.report.evicted_slots;
            const auto marker = citation_marker(slot);
            result.report.decisions.push_back({
                {"source_id_digest", source_digest(slot.source_id)},
                {"kind", memory_slot_kind_name(slot.kind)},
                {"reason", slot.text.empty() ? "budget_evicted" : "budget_truncated"},
                {"original_bytes", before}, {"kept_bytes", slot.text.size()},
                {"citation_retained", marker.empty() || slot.text.find(marker) != std::string::npos}});
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
