#include "agent/telemetry/slo.hpp"

#include <cmath>

namespace agent_framework::telemetry {
bool SloRegistry::register_objective(SloObjective value, std::string* error) {
    if (value.id.empty() || value.metric_name.empty() || !std::isfinite(value.threshold) ||
        value.minimum_samples == 0) {
        if (error) *error = "invalid SLO objective";
        return false;
    }
    return objectives_.emplace(value.id, std::move(value)).second;
}
SloReleaseDecision SloRegistry::evaluate(const std::vector<MetricResult>& metrics) const {
    SloReleaseDecision decision;
    for (const auto& [id, objective] : objectives_) {
        double weighted = 0.0; std::uint64_t samples = 0;
        for (const auto& metric : metrics) if (metric.metric_name == objective.metric_name &&
            std::isfinite(metric.value) && metric.sample_count > 0) {
            weighted += metric.value * static_cast<double>(metric.sample_count);
            samples += metric.sample_count;
        }
        SloEvaluation evaluation{id, "inconclusive", 0.0, samples, "insufficient samples"};
        if (samples >= objective.minimum_samples) {
            evaluation.observed = weighted / static_cast<double>(samples);
            const bool pass = objective.comparator == SloComparator::LessEqual
                ? evaluation.observed <= objective.threshold : evaluation.observed >= objective.threshold;
            evaluation.outcome = pass ? "passed" : "failed";
            evaluation.reason = pass ? "threshold satisfied" : "threshold violated";
        }
        if (objective.release_blocking && evaluation.outcome != "passed")
            decision.blockers.push_back("slo:" + id + ":" + evaluation.outcome);
        decision.evaluations.push_back(std::move(evaluation));
    }
    decision.allowed = !objectives_.empty() && decision.blockers.empty();
    return decision;
}
bool SloRegistry::register_error_budget(ErrorBudgetObjective value, std::string* error) {
    if(value.id.empty() || value.metric_name.empty() || value.target_good_ratio <= 0.0 ||
       value.target_good_ratio >= 1.0 || value.window_seconds == 0 ||
       !std::isfinite(value.good_threshold) || !std::isfinite(value.maximum_burn_rate) ||
       value.maximum_burn_rate <= 0.0) {
        if(error) *error="invalid error budget objective"; return false;
    }
    return error_budgets_.emplace(value.id,std::move(value)).second;
}
std::pair<bool,std::vector<ErrorBudgetEvaluation>> SloRegistry::evaluate_error_budgets(
    const std::vector<WindowedMetricSample>& samples,std::uint64_t now) const {
    bool allowed=!error_budgets_.empty(); std::vector<ErrorBudgetEvaluation> out;
    for(const auto&[id,o]:error_budgets_) { std::uint64_t total=0,good=0;
        const auto start=now>o.window_seconds?now-o.window_seconds:0;
        for(const auto&s:samples) if(s.metric_name==o.metric_name&&s.unix_time_seconds>=start&&s.unix_time_seconds<=now&&std::isfinite(s.value)){++total;if(s.value<=o.good_threshold)++good;}
        ErrorBudgetEvaluation e; e.objective_id=id;e.samples=total;
        if(total==0){e.outcome="inconclusive";if(o.release_blocking)allowed=false;out.push_back(e);continue;}
        e.good_ratio=static_cast<double>(good)/static_cast<double>(total);
        const auto allowed_bad=1.0-o.target_good_ratio;const auto observed_bad=1.0-e.good_ratio;
        e.budget_consumed=observed_bad/allowed_bad;e.burn_rate=e.budget_consumed;
        e.outcome=e.burn_rate<=o.maximum_burn_rate?"passed":"failed";
        if(o.release_blocking&&e.outcome!="passed")allowed=false;out.push_back(e);
    }
    return {allowed,out};
}
}  // namespace agent_framework::telemetry
