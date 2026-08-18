#include "agent/planning/evidence_store.hpp"

#include <algorithm>

namespace agent_framework::planning {

std::string InMemoryEvidenceStore::scope_key(const contracts::ContractMetadata& scope) {
    return scope.identity.tenant_id + '\x1f' + scope.identity.principal_id + '\x1f' +
           scope.identity.task_id;
}

PlanningCommitResult InMemoryEvidenceStore::append(
    const contracts::ContractMetadata& scope, const EvidenceRecord& record) {
    if(scope.identity.tenant_id.empty() || scope.identity.task_id.empty() ||
       record.evidence_id.empty() || record.content_digest.empty() || record.locator.empty())
        return {PlanningCommitStatus::Invalid, {}, "scope, id, locator, and digest are required"};
    if(record.instruction_authority &&
       (record.origin_kind == "external" || record.origin_kind == "rag" ||
        record.origin_kind == "web" || record.origin_kind == "tool"))
        return {PlanningCommitStatus::Invalid, {}, "untrusted evidence cannot carry instruction authority"};
    const auto key = scope_key(scope) + '\x1f' + record.evidence_id;
    std::lock_guard lock(mutex_);
    if(records_.count(key)) return {PlanningCommitStatus::Duplicate, record.content_digest, "evidence id exists"};
    const auto duplicate = std::find_if(records_.begin(), records_.end(), [&](const auto& item) {
        return item.first.starts_with(scope_key(scope) + '\x1f') &&
               item.second.content_digest == record.content_digest &&
               item.second.locator == record.locator;
    });
    if(duplicate != records_.end())
        return {PlanningCommitStatus::Duplicate, duplicate->second.content_digest, "evidence content exists"};
    records_.emplace(key, record);
    return {PlanningCommitStatus::Committed, record.content_digest, {}};
}

std::optional<EvidenceRecord> InMemoryEvidenceStore::get(
    const contracts::ContractMetadata& scope, std::string_view id) {
    std::lock_guard lock(mutex_);
    const auto found = records_.find(scope_key(scope) + '\x1f' + std::string(id));
    if(found == records_.end()) return std::nullopt;
    return found->second;
}

EvidenceBundle InMemoryEvidenceStore::bundle(
    const contracts::ContractMetadata& scope, const std::vector<std::string>& ids) {
    EvidenceBundle result;
    result.metadata = scope;
    std::lock_guard lock(mutex_);
    for(const auto& id : ids) {
        const auto found = records_.find(scope_key(scope) + '\x1f' + id);
        if(found != records_.end()) result.records.push_back(found->second);
    }
    std::sort(result.records.begin(), result.records.end(),
              [](const auto& left, const auto& right) { return left.evidence_id < right.evidence_id; });
    nlohmann::json basis = nlohmann::json::array();
    for(const auto& record : result.records)
        basis.push_back({record.evidence_id, record.content_digest});
    result.bundle_id = contracts::embedded_digest(basis).value_or("");
    return result;
}

}  // namespace agent_framework::planning
