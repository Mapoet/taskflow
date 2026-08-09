#include "agent/llm_runtime/router.hpp"

#include <algorithm>
#include <set>

namespace agent_framework::llm_runtime {
namespace {

bool contains(const std::vector<std::string>& values, std::string_view value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool includes_all(const std::vector<std::string>& available,
                  const std::vector<std::string>& required) {
    return std::all_of(required.begin(), required.end(), [&](const auto& item) {
        return contains(available, item);
    });
}

std::optional<double> estimate_cost(const ProviderModel& candidate,
                                    std::uint64_t input_tokens,
                                    int output_tokens) {
    if(!candidate.input_cost_per_million || !candidate.output_cost_per_million)
        return std::nullopt;
    return static_cast<double>(input_tokens) * *candidate.input_cost_per_million / 1000000.0 +
           static_cast<double>(output_tokens) * *candidate.output_cost_per_million / 1000000.0;
}

}  // namespace

bool ModelRouter::register_candidate(ProviderModel candidate) {
    if(candidate.candidate_id.empty() || candidate.provider.empty() || candidate.model.empty() ||
       candidate.adapter_revision.empty()) return false;
    std::lock_guard lock(mutex_);
    const auto found = candidates_.find(candidate.candidate_id);
    if(found != candidates_.end()) {
        return found->second.provider == candidate.provider &&
               found->second.model == candidate.model &&
               found->second.adapter_revision == candidate.adapter_revision;
    }
    candidates_.emplace(candidate.candidate_id, std::move(candidate));
    return true;
}

bool ModelRouter::set_available(std::string_view candidate_id, bool available) {
    std::lock_guard lock(mutex_);
    const auto found = candidates_.find(std::string(candidate_id));
    if(found == candidates_.end()) return false;
    found->second.available = available;
    return true;
}

std::optional<ProviderModel> ModelRouter::find(std::string_view candidate_id) const {
    std::lock_guard lock(mutex_);
    const auto found = candidates_.find(std::string(candidate_id));
    return found == candidates_.end() ? std::nullopt : std::optional<ProviderModel>(found->second);
}

std::vector<ProviderModel> ModelRouter::candidates() const {
    std::lock_guard lock(mutex_);
    std::vector<ProviderModel> result;
    for(const auto& [id, candidate] : candidates_) {
        (void)id;
        result.push_back(candidate);
    }
    return result;
}

ModelRouteDecision ModelRouter::route(
    const LLMRoleProfile& profile, const RoleInvocationRequest& request,
    const std::vector<std::string>& excluded_candidate_ids) const {
    ModelRouteDecision decision;
    decision.metadata = request.metadata;
    decision.route_id = request.invocation_id + ":route:" +
        std::to_string(excluded_candidate_ids.size() + 1);
    decision.profile_id = profile.profile_id;
    decision.profile_revision = profile.revision;
    decision.policy_revision = request.policy_revision;

    if(!profile.enabled) {
        decision.decision_code = "profile_disabled";
        decision.decision_message = "role profile is disabled";
        return decision;
    }
    if(!includes_all(request.granted_capabilities, profile.required_capabilities)) {
        decision.decision_code = "capability_denied";
        decision.decision_message = "request does not grant every role capability";
        return decision;
    }
    if(!profile.memory_view_profile.empty() &&
       request.memory_view.profile != profile.memory_view_profile) {
        decision.decision_code = "memory_view_denied";
        decision.decision_message = "memory view profile does not match role binding";
        return decision;
    }
    if(contains(request.independence.forbidden_groups, profile.independence_group)) {
        decision.decision_code = "independence_denied";
        decision.decision_message = "role independence group is forbidden for this invocation";
        return decision;
    }
    if(request.independence.require_provider_diversity &&
       request.independence.forbidden_providers.empty()) {
        decision.decision_code = "provider_diversity_evidence_missing";
        decision.decision_message =
            "provider diversity was required but no prior provider was supplied";
        return decision;
    }
    if(request.independence.require_model_diversity &&
       request.independence.forbidden_models.empty()) {
        decision.decision_code = "model_diversity_evidence_missing";
        decision.decision_message =
            "model diversity was required but no prior model was supplied";
        return decision;
    }
    if(!request.required_region.empty() && !profile.allowed_regions.empty() &&
       !contains(profile.allowed_regions, request.required_region)) {
        decision.decision_code = "residency_denied";
        decision.decision_message = "required region is not allowed by role profile";
        return decision;
    }

    std::vector<ProviderModel> candidates;
    {
        std::lock_guard lock(mutex_);
        for(const auto& candidate_id : profile.provider_pool) {
            const auto found = candidates_.find(candidate_id);
            if(found != candidates_.end()) candidates.push_back(found->second);
            else decision.rejected_candidates.push_back(candidate_id + ":not_registered");
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        return left.priority < right.priority;
    });
    for(const auto& candidate : candidates) {
        const auto reject = [&](const std::string& reason) {
            decision.rejected_candidates.push_back(candidate.candidate_id + ":" + reason);
        };
        if(contains(excluded_candidate_ids, candidate.candidate_id)) { reject("excluded"); continue; }
        if(!candidate.available) { reject("unavailable"); continue; }
        if(!includes_all(candidate.capabilities, profile.required_capabilities)) {
            reject("capability_mismatch"); continue;
        }
        if(profile.max_context_tokens > 0 && candidate.max_context_tokens > 0 &&
           candidate.max_context_tokens < profile.max_context_tokens) {
            reject("context_too_small"); continue;
        }
        if(!request.required_region.empty() && !candidate.regions.empty() &&
           !contains(candidate.regions, request.required_region)) {
            reject("residency_mismatch"); continue;
        }
        if(contains(request.independence.forbidden_providers, candidate.provider) ||
           contains(request.independence.forbidden_models, candidate.model)) {
            reject("independence_mismatch"); continue;
        }
        const auto cost = estimate_cost(candidate, request.estimated_input_tokens,
                                        profile.max_output_tokens);
        if(profile.max_cost_usd) {
            if(!cost) { reject("pricing_unknown"); continue; }
            if(*cost > *profile.max_cost_usd) { reject("cost_budget_exceeded"); continue; }
        }
        decision.selected = true;
        decision.candidate_id = candidate.candidate_id;
        decision.provider = candidate.provider;
        decision.model = candidate.model;
        decision.adapter_revision = candidate.adapter_revision;
        decision.estimated_cost_usd = cost;
        decision.decision_code = "selected";
        decision.decision_message = "candidate satisfies deterministic route policy";
        return decision;
    }
    decision.decision_code = "no_eligible_candidate";
    decision.decision_message = "no provider model satisfies role and request policy";
    return decision;
}

}  // namespace agent_framework::llm_runtime
