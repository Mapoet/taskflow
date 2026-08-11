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
}  // namespace agent_framework::telemetry
