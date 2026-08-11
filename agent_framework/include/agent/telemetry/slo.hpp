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

class SloRegistry {
public:
    bool register_objective(SloObjective objective, std::string* error = nullptr);
    SloReleaseDecision evaluate(const std::vector<MetricResult>& metrics) const;
private:
    std::map<std::string, SloObjective> objectives_;
};
}  // namespace agent_framework::telemetry
