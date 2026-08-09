#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "agent/contracts/contract.hpp"

namespace agent_framework::telemetry {

struct CorrelationContext {
    contracts::ContractMetadata metadata;
    std::string trace_id;
    std::string span_id;
    std::string parent_span_id;
    std::string node_id;
    std::string evidence_id;
    std::string artifact_digest;
    std::string approval_id;
    std::string memory_snapshot_id;
    std::string memory_view_digest;
    std::string sandbox_id;
};

struct MetricResult {
    contracts::ContractMetadata metadata;
    std::string metric_name;
    double value{0.0};
    std::string unit;
    std::string threshold;
    std::string outcome;
    std::uint64_t sample_count{0};
    std::vector<std::string> evidence_ids;
};

nlohmann::json encode(const CorrelationContext& value);
nlohmann::json encode(const MetricResult& value);
std::optional<CorrelationContext> decode_correlation_context(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<MetricResult> decode_metric_result(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);

}  // namespace agent_framework::telemetry
