#pragma once

#include <map>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "agent/eval/judge_workflow.hpp"

namespace agent_framework::eval {

struct DatasetLayerManifest {
    DatasetLayer layer{DatasetLayer::Unit};
    std::vector<std::string> case_ids;
    std::vector<std::string> case_digests;
};
struct DatasetManifest {
    contracts::ContractMetadata metadata;
    std::string dataset_id;
    std::string version;
    std::string license;
    std::vector<DatasetLayerManifest> layers;
    std::string created_at;
};
struct DatasetManifestValidation {
    bool valid{false};
    std::vector<std::string> issues;
    std::string digest;
};

struct HumanLabel {
    contracts::ContractMetadata metadata;
    std::string label_id;
    std::string dataset_id;
    std::string dataset_version;
    std::string case_id;
    std::string criterion_id;
    std::string reviewer_id;
    std::string reviewer_role;
    std::string categorical_label;
    double score{0.0};
    std::vector<std::string> evidence_ids;
    std::string created_at;
};
struct InterRaterBaseline {
    std::string dataset_id;
    std::string dataset_version;
    std::uint64_t paired_items{0};
    double raw_agreement{0.0};
    double cohens_kappa{0.0};
    std::vector<std::string> disputed_item_ids;
    std::string digest;
};

struct FlakyPolicy {
    std::uint64_t minimum_runs{3};
    double maximum_failure_rate{0.2};
    double maximum_metric_coefficient_of_variation{0.1};
};
struct CaseHistory {
    std::string case_id;
    std::vector<bool> passed;
    std::map<std::string, std::vector<double>> metric_samples;
};
struct QuarantineDecision {
    std::string case_id;
    bool quarantined{false};
    std::vector<std::string> reasons;
    std::string digest;
};

struct CampaignSpec {
    contracts::ContractMetadata metadata;
    std::string campaign_id;
    std::string dataset_id;
    std::string dataset_version;
    std::string dataset_manifest_digest;
    std::string baseline_revision_digest;
    std::string candidate_revision_digest;
    std::string model_digest;
    std::string prompt_digest;
    std::string profile_digest;
    std::string suite_digest;
    std::string scheduled_at;
};
struct CampaignSignature {
    std::string algorithm;
    std::string key_id;
    std::string signed_digest;
    std::string signature;
};
struct CampaignReport {
    contracts::ContractMetadata metadata;
    std::string campaign_id;
    std::string dataset_manifest_digest;
    std::string spec_digest;
    bool passed{false};
    std::map<std::string, double> metrics;
    std::vector<std::string> quarantined_case_ids;
    std::vector<std::string> blockers;
    std::string completed_at;
    CampaignSignature signature;
};
struct CampaignLease {
    std::string campaign_id;
    std::string owner;
    std::int64_t expires_at_epoch{0};
};
struct CampaignTrend {
    std::string metric;
    double previous{0.0};
    double current{0.0};
    double delta{0.0};
};

std::string campaign_spec_digest(const CampaignSpec& value);
std::string campaign_report_signing_digest(const CampaignReport& value);
bool validate_campaign_spec(const CampaignSpec& value, const DatasetManifest& manifest,
                            std::string* error = nullptr);
bool validate_campaign_report(
    const CampaignReport& value,
    const std::function<bool(const CampaignSignature&)>& verifier,
    std::string* error = nullptr);
std::vector<CampaignTrend> compare_campaign_reports(const CampaignReport& previous,
                                                    const CampaignReport& current);

class SQLiteCampaignStore {
public:
    explicit SQLiteCampaignStore(std::string path, int busy_timeout_ms = 3000);
    ~SQLiteCampaignStore();
    SQLiteCampaignStore(const SQLiteCampaignStore&) = delete;
    SQLiteCampaignStore& operator=(const SQLiteCampaignStore&) = delete;
    bool put_spec(const CampaignSpec& value, std::string* error = nullptr);
    bool put_manifest(const DatasetManifest& value, std::string* error = nullptr);
    bool put_label(const HumanLabel& value, std::string* error = nullptr);
    bool put_quarantine(std::string_view campaign_id, const QuarantineDecision& value,
                        std::string* error = nullptr);
    bool put_report(const CampaignReport& value, std::string* error = nullptr);
    std::optional<CampaignSpec> load_spec(std::string_view campaign_id) const;
    std::optional<CampaignReport> load_report(std::string_view campaign_id) const;
    std::vector<CampaignReport> report_history(std::string_view dataset_id,
                                               std::string_view dataset_version) const;
    bool acquire_lease(const CampaignLease& lease, std::int64_t now_epoch,
                       std::string* error = nullptr);
    bool release_lease(std::string_view campaign_id, std::string_view owner);
private:
    void migrate();
    void* db_{nullptr};
    mutable std::mutex mutex_;
};

DatasetManifestValidation validate_manifest(const DatasetManifest& manifest,
                                            const DatasetRegistry& registry);
std::optional<InterRaterBaseline> calculate_inter_rater_baseline(
    const std::vector<HumanLabel>& labels, std::string* error = nullptr);
QuarantineDecision evaluate_flakiness(const CaseHistory& history,
                                      const FlakyPolicy& policy);

nlohmann::json encode(const DatasetManifest& value);
nlohmann::json encode(const HumanLabel& value);
nlohmann::json encode(const InterRaterBaseline& value);
nlohmann::json encode(const QuarantineDecision& value);
nlohmann::json encode(const CampaignSpec& value);
nlohmann::json encode(const CampaignReport& value);
}  // namespace agent_framework::eval
