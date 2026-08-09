#include <algorithm>
#include <cassert>
#include <string>

#include "phase4_llm_runtime_test_support.hpp"

namespace {
bool rejected_with(const agent_framework::llm_runtime::ModelRouteDecision& decision,
                   const std::string& suffix) {
    return std::any_of(decision.rejected_candidates.begin(),
        decision.rejected_candidates.end(), [&](const auto& value) {
            return value.find(suffix) != std::string::npos;
        });
}
}

int main() {
    using namespace phase4_llm_test;
    ModelRouter router;
    assert(router.register_candidate(candidate("primary", "provider-a", "model-a", 10)));
    assert(router.register_candidate(candidate("fallback", "provider-b", "model-b", 20)));
    assert(router.register_candidate(candidate("primary", "provider-a", "model-a", 10)));

    auto p = profile();
    auto r = request();
    auto decision = router.route(p, r);
    assert(decision.selected && decision.candidate_id == "primary");
    assert(decision.estimated_cost_usd && *decision.estimated_cost_usd < 0.10);

    r.independence.require_provider_diversity = true;
    decision = router.route(p, r);
    assert(!decision.selected && decision.decision_code == "provider_diversity_evidence_missing");
    r.independence.forbidden_providers = {"provider-a"};
    decision = router.route(p, r);
    assert(decision.selected && decision.candidate_id == "fallback");
    assert(rejected_with(decision, "independence_mismatch"));

    r = request();
    r.independence.require_model_diversity = true;
    assert(router.route(p, r).decision_code == "model_diversity_evidence_missing");
    r = request();
    r.independence.forbidden_groups = {"planner"};
    assert(router.route(p, r).decision_code == "independence_denied");

    r = request();
    r.required_region = "eu";
    assert(router.route(p, r).decision_code == "residency_denied");
    r = request();
    r.granted_capabilities.clear();
    assert(router.route(p, r).decision_code == "capability_denied");

    r = request();
    p.max_cost_usd = 0.000001;
    decision = router.route(p, r);
    assert(!decision.selected && rejected_with(decision, "cost_budget_exceeded"));

    p = profile();
    assert(router.set_available("primary", false));
    decision = router.route(p, request());
    assert(decision.selected && decision.candidate_id == "fallback");
    assert(rejected_with(decision, "unavailable"));
    assert(router.set_available("fallback", false));
    assert(router.route(p, request()).decision_code == "no_eligible_candidate");
    return 0;
}
