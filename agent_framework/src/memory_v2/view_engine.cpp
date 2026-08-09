#include "agent/memory_v2/view_engine.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "agent/contracts/contract.hpp"

namespace agent_framework::memory_v2 {
namespace {

bool contains_kind(MemoryKind value, const std::vector<MemoryKind>& allowed) {
    return allowed.empty() || std::find(allowed.begin(), allowed.end(), value) != allowed.end();
}
bool mandatory(const std::string& id, const MemoryViewSpec& spec) {
    return std::find(spec.mandatory_record_ids.begin(), spec.mandatory_record_ids.end(), id) !=
           spec.mandatory_record_ids.end();
}
std::size_t path_depth(const std::string& value) {
    return static_cast<std::size_t>(std::count(value.begin(), value.end(), '/'));
}
std::uint64_t mix_generation(std::uint64_t state, std::string_view value) {
    constexpr std::uint64_t prime = 1099511628211ULL;
    if(state == 0) state = 1469598103934665603ULL;
    for(unsigned char ch : value) state = (state ^ ch) * prime;
    return state;
}
MemorySelection selection(const MemoryRecord& record, std::string reason,
                          std::uint64_t bytes) {
    auto document = encode(record);
    return {record.record_id, record.revision,
            document.at("canonical_digest").get<std::string>(), std::move(reason), bytes};
}

}  // namespace

MemoryView MemoryViewEngine::build(const MemoryViewSpec& spec, std::string_view now) {
    MemoryView out;
    out.snapshot.metadata = spec.metadata;
    out.manifest.metadata = spec.metadata;
    MemoryQuery query{spec.subject, spec.metadata.identity.principal_id,
                      spec.allowed_levels, 10000};
    auto batches = providers_.fetch_all(query);
    std::map<std::string, MemoryRecord> unique;
    std::uint64_t generation = 0;
    for(auto& batch : batches) {
        if(!batch.error.empty()) {
            out.fail_closed = true;
            out.error = "provider_failure:" + batch.provider_id;
            return out;
        }
        generation = mix_generation(generation, batch.provider_id);
        generation = mix_generation(generation, std::to_string(batch.generation));
        for(auto& record : batch.records) {
            // Provider adapters are not trusted to implement tenant/scope/ACL filtering.
            // Enforce the same visibility predicate again before ranking or materialization.
            if(record.record_id.empty() ||
               record.metadata.identity.tenant_id != record.scope.tenant_id ||
               !memory_visible_to(record, query)) {
                out.fail_closed = true;
                out.error = "provider_scope_violation:" + batch.provider_id;
                return out;
            }
            auto found = unique.find(record.record_id);
            if(found == unique.end() || found->second.revision < record.revision)
                unique[record.record_id] = std::move(record);
        }
    }
    std::vector<MemoryRecord> candidates;
    for(auto& [id, record] : unique) { (void)id; candidates.push_back(std::move(record)); }
    std::sort(candidates.begin(), candidates.end(), [&spec](const auto& left, const auto& right) {
        if(mandatory(left.record_id, spec) != mandatory(right.record_id, spec))
            return mandatory(left.record_id, spec);
        if(left.authority != right.authority)
            return static_cast<int>(left.authority) < static_cast<int>(right.authority);
        if(left.scope.level != right.scope.level)
            return static_cast<int>(left.scope.level) < static_cast<int>(right.scope.level);
        if(path_depth(left.scope.path_scope) != path_depth(right.scope.path_scope))
            return path_depth(left.scope.path_scope) > path_depth(right.scope.path_scope);
        return left.record_id < right.record_id;
    });

    nlohmann::json snapshot_refs = nlohmann::json::array();
    for(const auto& record : candidates) {
        const auto digest = encode(record).at("canonical_digest").get<std::string>();
        out.snapshot.record_revision_digests.push_back(digest);
        snapshot_refs.push_back({record.record_id, record.revision, digest});
    }
    out.snapshot.provider_generation = generation;
    out.snapshot.policy_revision = spec.metadata.extensions.value("policy_revision", "");
    const auto snapshot_digest = contracts::embedded_digest(snapshot_refs).value_or("");
    out.snapshot.snapshot_id = snapshot_digest;
    out.manifest.snapshot_id = snapshot_digest;
    out.manifest.snapshot_digest = snapshot_digest;
    out.manifest.provider_generation = generation;
    out.manifest.policy_revision = out.snapshot.policy_revision;
    out.manifest.view_spec_digest = encode(spec).at("canonical_digest").get<std::string>();

    std::set<std::string> selected_ids;
    std::uint64_t used = 0;
    for(const auto& record : candidates) {
        const auto bytes = static_cast<std::uint64_t>(contracts::canonical_json(encode(record)).size());
        std::string reason;
        if(!contains_kind(record.kind, spec.allowed_kinds)) reason = "kind_not_allowed";
        else if(spec.exclude_unverified_executor_claims &&
                (record.source_kind == "executor_claim" ||
                 record.trust_class == "unverified_executor"))
            reason = "verification_isolation";
        else if(static_cast<int>(record.authority) > static_cast<int>(spec.authority_floor))
            reason = "authority_below_floor";
        else if(!record.freshness_deadline.empty() && !now.empty() &&
                record.freshness_deadline < std::string(now)) reason = "stale";
        else {
            for(const auto& conflict : record.conflicts_with) {
                if(selected_ids.count(conflict)) {
                    reason = "conflict_with_selected:" + conflict;
                    out.manifest.conflict_ids.push_back(record.record_id + ":" + conflict);
                    break;
                }
            }
        }
        if(reason.empty() && spec.byte_budget != 0 && used + bytes > spec.byte_budget)
            reason = "byte_budget";
        if(reason.empty() && spec.token_budget != 0 && (used + bytes + 3) / 4 > spec.token_budget)
            reason = "token_budget";
        if(!reason.empty()) {
            out.manifest.excluded.push_back(selection(record, reason, bytes));
            if(mandatory(record.record_id, spec)) {
                out.fail_closed = true;
                out.error = "mandatory_memory_excluded:" + record.record_id;
            }
            continue;
        }
        used += bytes;
        selected_ids.insert(record.record_id);
        out.records.push_back(record);
        out.manifest.selected.push_back(selection(record, "selected", bytes));
    }
    for(const auto& id : spec.mandatory_record_ids) {
        if(!selected_ids.count(id)) {
            out.fail_closed = true;
            if(out.error.empty()) out.error = "mandatory_memory_missing:" + id;
        }
    }
    nlohmann::json view_basis = {{"snapshot_digest", out.manifest.snapshot_digest},
        {"view_spec_digest", out.manifest.view_spec_digest},
        {"selected", nlohmann::json::array()}, {"excluded", nlohmann::json::array()},
        {"policy_revision", out.manifest.policy_revision}, {"provider_generation", generation}};
    for(const auto& item : out.manifest.selected)
        view_basis["selected"].push_back({item.record_id, item.revision, item.digest,
                                           item.reason, item.bytes});
    for(const auto& item : out.manifest.excluded)
        view_basis["excluded"].push_back({item.record_id, item.revision, item.digest,
                                           item.reason, item.bytes});
    out.manifest.view_digest = contracts::embedded_digest(view_basis).value_or("");
    return out;
}

}  // namespace agent_framework::memory_v2
