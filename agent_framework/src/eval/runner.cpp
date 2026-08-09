#include "agent/eval/runner.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "agent/contracts/contract.hpp"

namespace agent_framework::eval {

bool DatasetRegistry::register_case(DatasetCase item, std::string* error) {
    if(item.case_id.empty() || item.dataset_version.empty() || item.license.empty() ||
       item.metadata.identity.tenant_id.empty()) {
        if(error) *error = "case id, dataset version, license, and tenant are required";
        return false;
    }
    std::lock_guard lock(mutex_);
    const auto found = cases_.find(item.case_id);
    if(found != cases_.end()) {
        if(error) *error = encode(found->second).at("canonical_digest") ==
                          encode(item).at("canonical_digest") ? "case already registered" :
                          "immutable dataset case revision conflict";
        return false;
    }
    cases_.emplace(item.case_id, std::move(item));
    return true;
}

std::optional<DatasetCase> DatasetRegistry::case_for_scoring(std::string_view id) const {
    std::lock_guard lock(mutex_);
    const auto found = cases_.find(std::string(id));
    return found == cases_.end() ? std::nullopt : std::optional<DatasetCase>(found->second);
}

std::vector<EvalInput> DatasetRegistry::inputs(std::uint64_t seed) const {
    std::lock_guard lock(mutex_);
    std::vector<EvalInput> result;
    std::uint64_t offset = 0;
    for(const auto& [id, item] : cases_)
        result.push_back({id, item.input, item.environment, seed + offset++});
    return result;
}

EvalRunResult EvalRunner::run(
    const DatasetRegistry& registry, std::uint64_t seed, const CaseExecutor& executor) const {
    EvalRunResult result;
    for(const auto& input : registry.inputs(seed)) result.cases.push_back(executor(input));
    nlohmann::json basis = nlohmann::json::array();
    for(const auto& item : result.cases) {
        nlohmann::json metrics = nlohmann::json::object();
        for(const auto& [name, value] : item.metrics) metrics[name] = value;
        basis.push_back({item.trajectory.case_id,
                         encode(item.trajectory).at("canonical_digest"), metrics, item.error});
    }
    result.run_digest = contracts::embedded_digest(basis).value_or("");
    return result;
}

std::vector<MetricComparison> compare(
    const EvalRunResult& baseline, const EvalRunResult& candidate,
    const std::vector<RegressionGate>& gates) {
    std::vector<MetricComparison> result;
    for(const auto& gate : gates) {
        std::vector<double> base, next, delta;
        const auto count = std::min(baseline.cases.size(), candidate.cases.size());
        for(std::size_t i = 0; i < count; ++i) {
            const auto b = baseline.cases[i].metrics.find(gate.metric);
            const auto c = candidate.cases[i].metrics.find(gate.metric);
            if(b == baseline.cases[i].metrics.end() || c == candidate.cases[i].metrics.end()) continue;
            base.push_back(b->second); next.push_back(c->second); delta.push_back(c->second - b->second);
        }
        MetricComparison item;
        item.metric = gate.metric;
        if(base.empty()) { item.regression = true; result.push_back(item); continue; }
        item.baseline_mean = std::accumulate(base.begin(), base.end(), 0.0) / base.size();
        item.candidate_mean = std::accumulate(next.begin(), next.end(), 0.0) / next.size();
        item.paired_delta = std::accumulate(delta.begin(), delta.end(), 0.0) / delta.size();
        if(delta.size() > 1) {
            double sum = 0;
            for(double value : delta) sum += (value - item.paired_delta) * (value - item.paired_delta);
            const double standard_error = std::sqrt(sum / (delta.size() - 1)) / std::sqrt(delta.size());
            item.ci95_half_width = 1.96 * standard_error;
        }
        item.regression = gate.higher_is_better
            ? item.paired_delta + item.ci95_half_width < -gate.maximum_regression
            : item.paired_delta - item.ci95_half_width > gate.maximum_regression;
        result.push_back(item);
    }
    return result;
}

}  // namespace agent_framework::eval
