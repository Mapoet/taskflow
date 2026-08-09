#include "agent/memory_v2/governance.hpp"

#include <algorithm>
#include <stdexcept>

namespace agent_framework::memory_v2 {

MemoryGovernanceService::MemoryGovernanceService(std::shared_ptr<MemoryStore> store)
    : store_(std::move(store)) {
    if(!store_) throw std::invalid_argument("memory store is required");
}

bool MemoryGovernanceService::register_forget_sink(std::shared_ptr<ForgetSink> sink) {
    if(!sink || sink->id().empty()) return false;
    if(std::any_of(sinks_.begin(), sinks_.end(), [&](const auto& item) { return item->id() == sink->id(); }))
        return false;
    sinks_.push_back(std::move(sink));
    return true;
}

CommitResult MemoryGovernanceService::promote(
    std::string_view id, std::uint64_t expected, MemoryStatus status, Authority authority,
    std::string_view decision, std::string_view approval) {
    if(decision.empty()) return {CommitStatus::Invalid, expected, "promotion decision is required"};
    auto current = store_->current(id);
    if(!current) return {CommitStatus::NotFound, 0, "record not found"};
    if(current->revision != expected)
        return {CommitStatus::RevisionConflict, current->revision, "revision conflict"};
    if((status == MemoryStatus::Verified && authority != Authority::Verified) ||
       (status == MemoryStatus::Authoritative && authority != Authority::Authoritative))
        return {CommitStatus::Invalid, expected, "status and authority do not match"};
    const bool human_gate = status == MemoryStatus::Authoritative ||
        current->scope.level == MemoryLevel::System ||
        current->scope.level == MemoryLevel::Organization ||
        current->scope.level == MemoryLevel::Principal;
    if(human_gate && approval.empty())
        return {CommitStatus::Forbidden, expected, "promotion requires approval or consent"};
    current->revision = expected + 1;
    current->status = status;
    current->authority = authority;
    current->metadata.extensions["promotion_decision_id"] = std::string(decision);
    return store_->revise(*current, expected, approval);
}

GovernanceResult MemoryGovernanceService::forget(
    std::string_view id, std::uint64_t expected, std::string_view approval) {
    GovernanceResult result;
    if(approval.empty()) {
        result.commit = {CommitStatus::Forbidden, expected, "forget requires approval"};
        return result;
    }
    auto current = store_->current(id);
    if(!current) {
        result.commit = {CommitStatus::NotFound, 0, "record not found"};
        return result;
    }
    if(current->revision != expected) {
        result.commit = {CommitStatus::RevisionConflict, current->revision, "revision conflict"};
        return result;
    }
    if(current->legal_hold) {
        result.commit = {CommitStatus::Forbidden, expected, "legal hold prevents forgetting"};
        return result;
    }
    current->revision = expected + 1;
    current->status = MemoryStatus::Tombstoned;
    current->content = {{"forgotten", true}};
    current->source_locator.clear();
    current->source_digest.clear();
    current->metadata.extensions["forget_approval_id"] = std::string(approval);
    result.commit = store_->revise(*current, expected, approval);
    if(!result.commit) return result;
    for(const auto& sink : sinks_) {
        std::string error;
        if(sink->erase(id, &error)) result.completed_sinks.push_back(sink->id());
        else result.inconclusive_sinks.push_back(sink->id() + (error.empty() ? "" : ":" + error));
    }
    return result;
}

}  // namespace agent_framework::memory_v2
