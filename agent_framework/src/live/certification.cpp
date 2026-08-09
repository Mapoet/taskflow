#include "agent/live/certification.hpp"

namespace agent_framework::live {

std::string environment_digest(const EnvironmentManifest& value) {
    nlohmann::json secret_refs = value.secret_refs;
    return contracts::embedded_digest({
        {"identity", contracts::identity_to_json(value.metadata.identity)},
        {"os", value.os}, {"build_digest", value.build_digest},
        {"git_revision", value.git_revision}, {"config_digest", value.config_digest},
        {"provider_revision", value.provider_revision}, {"endpoint_class", value.endpoint_class},
        {"memory_policy_revision", value.memory_policy_revision},
        {"provider_generation", value.provider_generation}, {"secret_refs", secret_refs}}).value_or("");
}

CertificationReport certify(const EnvironmentManifest& manifest,
                            std::vector<ExecutionAttestation> cells,
                            std::string issued, std::string expires) {
    CertificationReport report;
    report.environment_digest = environment_digest(manifest);
    report.issued_at = std::move(issued);
    report.expires_at = std::move(expires);
    report.cells = std::move(cells);
    for(const auto& cell : report.cells) {
        if(cell.required && !cell.executed)
            report.blockers.push_back(cell.cell_id + ":required_not_executed:" + cell.reason);
        else if(cell.required && !cell.passed)
            report.blockers.push_back(cell.cell_id + ":failed:" + cell.reason);
        else if(cell.executed && cell.evidence_digests.empty())
            report.blockers.push_back(cell.cell_id + ":missing_evidence");
    }
    if(report.cells.empty()) report.blockers.push_back("matrix_empty");
    if(report.environment_digest.empty() || report.expires_at.empty())
        report.blockers.push_back("manifest_or_expiry_missing");
    report.certified = report.blockers.empty();
    return report;
}

bool certification_valid_for(const CertificationReport& report,
                             const EnvironmentManifest& manifest,
                             std::string_view now) {
    return report.certified && report.environment_digest == environment_digest(manifest) &&
           !report.expires_at.empty() && (now.empty() || report.expires_at >= now);
}

}  // namespace agent_framework::live
