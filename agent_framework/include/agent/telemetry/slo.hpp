#pragma once

#include <map>
#include <string>
#include <vector>

#include "agent/telemetry/types.hpp"

namespace agent_framework::telemetry {

enum class SloComparator { LessEqual, GreaterEqual };
struct SloObjective {
    std::string id;
    std::string metric_name;
    SloComparator comparator{SloComparator::LessEqual};
    double threshold{0.0};
    std::uint64_t minimum_samples{1};
    bool release_blocking{true};
};
struct SloEvaluation {
    std::string objective_id;
    std::string outcome;
    double observed{0.0};
    std::uint64_t sample_count{0};
    std::string reason;
};
struct SloReleaseDecision {
    bool allowed{false};
    std::vector<SloEvaluation> evaluations;
    std::vector<std::string> blockers;
};
struct WindowedMetricSample {
    std::string metric_name;
    double value{0.0};
    std::uint64_t unix_time_seconds{0};
};
struct QuantileObjective {
    std::string id;
    std::string metric_name;
    double quantile{0.95};
    double threshold{0.0};
    std::uint64_t window_seconds{3600};
    std::uint64_t minimum_samples{1};
    bool release_blocking{true};
};
struct ErrorBudgetObjective {
    std::string id;
    std::string metric_name;
    double target_good_ratio{0.99};
    double good_threshold{0.0};
    std::uint64_t window_seconds{3600};
    double maximum_burn_rate{1.0};
    bool release_blocking{true};
    std::vector<std::uint64_t> alert_windows_seconds;
};
struct ErrorBudgetEvaluation {
    std::string objective_id;
    double good_ratio{0.0};
    double budget_consumed{0.0};
    double burn_rate{0.0};
    std::uint64_t samples{0};
    std::string outcome;
    std::map<std::uint64_t, double> window_burn_rates;
};

class SloRegistry {
public:
    bool register_objective(SloObjective objective, std::string* error = nullptr);
    SloReleaseDecision evaluate(const std::vector<MetricResult>& metrics) const;
    bool register_error_budget(ErrorBudgetObjective objective, std::string* error = nullptr);
    bool register_quantile(QuantileObjective objective, std::string* error = nullptr);
    SloReleaseDecision evaluate_quantiles(const std::vector<WindowedMetricSample>& samples,
                                          std::uint64_t now_seconds) const;
    std::pair<bool, std::vector<ErrorBudgetEvaluation>> evaluate_error_budgets(
        const std::vector<WindowedMetricSample>& samples,
        std::uint64_t now_seconds) const;
private:
    std::map<std::string, SloObjective> objectives_;
    std::map<std::string, ErrorBudgetObjective> error_budgets_;
    std::map<std::string, QuantileObjective> quantiles_;
};
}  // namespace agent_framework::telemetry
