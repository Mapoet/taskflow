#pragma once

#include <optional>
#include <string>
#include <vector>

#include "agent/contracts/contract.hpp"

namespace agent_framework::eval {

struct DatasetCase {
    contracts::ContractMetadata metadata;
    std::string case_id;
    std::string input;
    nlohmann::json environment = nlohmann::json::object();
    nlohmann::json ground_truth = nlohmann::json::object();
    std::vector<std::string> criterion_ids;
    std::vector<std::string> tags;
    std::string license;
    std::string dataset_version;
};

struct Trajectory {
    contracts::ContractMetadata metadata;
    std::string case_id;
    std::string component_version;
    std::string environment_digest;
    std::vector<std::string> event_digests;
    std::string acceptance_report_digest;
    std::string started_at;
    std::string finished_at;
};

struct ComparisonResult {
    contracts::ContractMetadata metadata;
    std::string baseline_version;
    std::string candidate_version;
    std::vector<std::string> metric_result_digests;
    std::string decision;
    std::vector<std::string> regressions;
};

nlohmann::json encode(const DatasetCase& value);
nlohmann::json encode(const Trajectory& value);
nlohmann::json encode(const ComparisonResult& value);
std::optional<DatasetCase> decode_dataset_case(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<Trajectory> decode_trajectory(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<ComparisonResult> decode_comparison_result(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);

}  // namespace agent_framework::eval
