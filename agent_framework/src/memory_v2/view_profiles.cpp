#include "agent/memory_v2/view_profiles.hpp"

#include <array>

namespace agent_framework::memory_v2 {
namespace {
using L = MemoryLevel;
using K = MemoryKind;

void assign_profile(MemoryViewSpec& spec, MemoryViewMode mode) {
    switch(mode) {
    case MemoryViewMode::Intake:
        spec.allowed_levels = {L::System, L::Organization, L::Principal, L::Project, L::Task, L::Turn};
        spec.allowed_kinds = {K::Instruction, K::Procedural, K::Semantic, K::Conversational};
        break;
    case MemoryViewMode::Investigation:
        spec.allowed_levels = {L::System, L::Organization, L::Principal, L::Project, L::Task, L::Turn};
        spec.allowed_kinds = {K::Instruction, K::Semantic, K::Episodic, K::Procedural,
                              K::Evidentiary, K::Operational};
        break;
    case MemoryViewMode::Planning:
        spec.allowed_levels = {L::System, L::Organization, L::Principal, L::Project, L::Task, L::Turn};
        spec.allowed_kinds = {K::Instruction, K::Semantic, K::Procedural, K::Evidentiary,
                              K::Operational, K::Conversational};
        break;
    case MemoryViewMode::Execution:
        spec.allowed_levels = {L::System, L::Organization, L::Project, L::Task, L::Turn};
        spec.allowed_kinds = {K::Instruction, K::Procedural, K::Evidentiary,
                              K::Operational, K::Working};
        break;
    case MemoryViewMode::Verification:
        spec.allowed_levels = {L::System, L::Organization, L::Project, L::Task};
        spec.allowed_kinds = {K::Instruction, K::Procedural, K::Evidentiary, K::Operational};
        spec.include_procedural_skills = false;
        spec.exclude_unverified_executor_claims = true;
        break;
    case MemoryViewMode::Replan:
        spec.allowed_levels = {L::System, L::Organization, L::Principal, L::Project, L::Task, L::Turn};
        spec.allowed_kinds = {K::Instruction, K::Semantic, K::Episodic, K::Evidentiary,
                              K::Operational, K::Conversational};
        break;
    case MemoryViewMode::Resume:
        spec.allowed_levels = {L::System, L::Organization, L::Project, L::Task, L::Turn};
        spec.allowed_kinds = {K::Instruction, K::Procedural, K::Evidentiary,
                              K::Operational, K::Working, K::Conversational};
        break;
    case MemoryViewMode::Handoff:
        spec.allowed_levels = {L::System, L::Organization, L::Project, L::Task};
        spec.allowed_kinds = {K::Instruction, K::Procedural, K::Evidentiary, K::Operational};
        spec.exclude_unverified_executor_claims = true;
        break;
    }
}
}  // namespace

std::string_view to_string(MemoryViewMode mode) noexcept {
    static constexpr std::array<std::string_view, 8> values = {
        "intake", "investigation", "planning", "execution",
        "verification", "replan", "resume", "handoff"};
    return values[static_cast<std::size_t>(mode)];
}

std::optional<MemoryViewMode> memory_view_mode(std::string_view value) noexcept {
    for(int i = 0; i != 8; ++i)
        if(to_string(static_cast<MemoryViewMode>(i)) == value)
            return static_cast<MemoryViewMode>(i);
    return std::nullopt;
}

MemoryViewSpec make_view_spec(MemoryViewMode mode, contracts::ContractMetadata metadata,
                              MemoryScope subject, ViewBudget budget) {
    MemoryViewSpec spec;
    spec.metadata = std::move(metadata);
    spec.workflow_phase = std::string(to_string(mode));
    spec.subject = std::move(subject);
    spec.authority_floor = Authority::Derived;
    spec.byte_budget = budget.bytes;
    spec.token_budget = budget.tokens;
    spec.conflict_policy = "authority-specificity-freshness/fail-closed";
    assign_profile(spec, mode);
    return spec;
}

std::optional<MemoryViewMode> MemoryViewRouter::route(
    MemoryViewMode current, std::string_view event) const noexcept {
    if(event == "task_received") return MemoryViewMode::Intake;
    if(event == "investigation_started") return MemoryViewMode::Investigation;
    if(event == "planning_started" || event == "plan_drafting") return MemoryViewMode::Planning;
    if(event == "plan_approved" || event == "node_started") return MemoryViewMode::Execution;
    if(event == "verification_started") return MemoryViewMode::Verification;
    if(event == "finding_requires_replan" || event == "execution_failed") return MemoryViewMode::Replan;
    if(event == "run_resumed") return MemoryViewMode::Resume;
    if(event == "handoff_started") return MemoryViewMode::Handoff;
    if(event == "no_change") return current;
    return std::nullopt;
}

}  // namespace agent_framework::memory_v2
