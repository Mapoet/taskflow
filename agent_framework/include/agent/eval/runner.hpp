#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "agent/eval/types.hpp"

namespace agent_framework::eval {

struct EvalInput {
    std::string case_id;
    std::string input;
    nlohmann::json environment;
    std::uint64_t seed{0};
};
struct CaseResult {
    Trajectory trajectory;
    std::map<std::string, double> metrics;
    std::string error;
};
using CaseExecutor = std::function<CaseResult(const EvalInput&)>;

class DatasetRegistry {
public:
    bool register_case(DatasetCase dataset_case, std::string* error = nullptr);
    std::optional<DatasetCase> case_for_scoring(std::string_view case_id) const;
    std::vector<EvalInput> inputs(std::uint64_t seed) const;
private:
    mutable std::mutex mutex_;
    std::map<std::string, DatasetCase> cases_;
};

struct EvalRunResult {
    std::vector<CaseResult> cases;
    std::string run_digest;
};

class EvalRunner {
public:
    EvalRunResult run(const DatasetRegistry& registry, std::uint64_t seed,
                      const CaseExecutor& executor) const;
};

struct RegressionGate {
    std::string metric;
    double maximum_regression{0.0};
    bool higher_is_better{true};
};
struct MetricComparison {
    std::string metric;
    double baseline_mean{0.0};
    double candidate_mean{0.0};
    double paired_delta{0.0};
    double ci95_half_width{0.0};
    bool regression{false};
};

std::vector<MetricComparison> compare(
    const EvalRunResult& baseline, const EvalRunResult& candidate,
    const std::vector<RegressionGate>& gates);

}  // namespace agent_framework::eval
