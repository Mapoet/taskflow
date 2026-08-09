#include "agent/planning/plan_store.hpp"

namespace agent_framework::planning {

std::string InMemoryPlanStore::key(const contracts::ContractIdentity& identity) {
    return identity.tenant_id + '\x1f' + identity.task_id + '\x1f' + identity.plan_id;
}

PlanningCommitResult InMemoryPlanStore::create(const ExecutionPlan& plan) {
    if(plan.metadata.identity.tenant_id.empty() || plan.metadata.identity.task_id.empty() ||
       plan.metadata.identity.plan_id.empty() || plan.plan_revision != 1 ||
       !plan.parent_plan_digest.empty())
        return {PlanningCommitStatus::Invalid, {}, "initial plan identity/revision is invalid"};
    const auto digest = encode(plan).at("canonical_digest").get<std::string>();
    std::lock_guard lock(mutex_);
    auto& versions = plans_[key(plan.metadata.identity)];
    if(!versions.empty()) return {PlanningCommitStatus::Duplicate, {}, "plan exists"};
    versions.push_back(plan);
    return {PlanningCommitStatus::Committed, digest, {}};
}

PlanningCommitResult InMemoryPlanStore::compare_exchange(
    const ExecutionPlan& plan, std::uint64_t expected) {
    std::lock_guard lock(mutex_);
    const auto found = plans_.find(key(plan.metadata.identity));
    if(found == plans_.end() || found->second.empty())
        return {PlanningCommitStatus::NotFound, {}, "plan not found"};
    const auto& current = found->second.back();
    if(current.plan_revision != expected)
        return {PlanningCommitStatus::RevisionConflict, {}, "plan revision conflict"};
    const auto parent_digest = encode(current).at("canonical_digest").get<std::string>();
    if(plan.plan_revision != expected + 1 || plan.parent_plan_digest != parent_digest)
        return {PlanningCommitStatus::Invalid, {}, "new revision must bind the current parent digest"};
    const auto digest = encode(plan).at("canonical_digest").get<std::string>();
    found->second.push_back(plan);
    return {PlanningCommitStatus::Committed, digest, {}};
}

std::optional<ExecutionPlan> InMemoryPlanStore::current(
    const contracts::ContractIdentity& identity) {
    std::lock_guard lock(mutex_);
    const auto found = plans_.find(key(identity));
    if(found == plans_.end() || found->second.empty()) return std::nullopt;
    return found->second.back();
}

std::vector<ExecutionPlan> InMemoryPlanStore::history(
    const contracts::ContractIdentity& identity) {
    std::lock_guard lock(mutex_);
    const auto found = plans_.find(key(identity));
    return found == plans_.end() ? std::vector<ExecutionPlan>{} : found->second;
}

}  // namespace agent_framework::planning
