#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "agent/live/role_certification.hpp"
#include "agent/live/production_approval.hpp"
#include "agent/live/production_signature.hpp"

namespace
{
    std::string required(const char *name)
    {
        const char *value = std::getenv(name);
        if (!value || !*value)
        {
            std::cerr << "BLOCKED: required Live setting is absent: " << name << '\n';
            std::exit(2);
        }
        return value;
    }
    std::string read_file(const std::string& path)
    {
        std::ifstream input(path);
        if(!input) return {};
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return buffer.str();
    }
}

int main()
{
    using namespace agent_framework::live;
    const auto report_path = required("AGENT_PHASE4_LIVE_REPORT");
    const auto expected_environment = required("AGENT_PHASE4_LIVE_EXPECTED_ENVIRONMENT_DIGEST");
    const auto expected_matrix = required("AGENT_PHASE4_LIVE_EXPECTED_MATRIX_DIGEST");
    const auto expected_key_id = required("AGENT_PHASE4_LIVE_SIGNING_KEY_ID");
    const auto public_key_path = required("AGENT_PHASE4_LIVE_PUBLIC_KEY_FILE");
    const auto expected_approval = required("AGENT_PHASE4_LIVE_APPROVAL_DECISION_ID");
    const auto approval_store_path = required("AGENT_PHASE4_LIVE_APPROVAL_STORE");
    const auto approval_tenant = required("AGENT_PHASE4_LIVE_APPROVAL_TENANT");
    const auto approval_scope = required("AGENT_PHASE4_LIVE_APPROVAL_SCOPE");
    const auto now = required("AGENT_PHASE4_LIVE_NOW");
    std::ifstream input(report_path);
    if (!input)
    {
        std::cerr << "BLOCKED: Live report was not produced: " << report_path << '\n';
        return 2;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    nlohmann::json document;
    try
    {
        document = nlohmann::json::parse(buffer.str());
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAILED: Live report JSON is invalid: " << e.what() << '\n';
        return 1;
    }
    auto report = decode_role_certification_report(document);
    if (!report)
    {
        std::cerr << "FAILED: Live report contract or canonical digest is invalid\n";
        return 1;
    }
    if (report->state != RoleCertificationState::Certified || !report->executed || !report->blockers.empty())
    {
        std::cerr << "FAILED: Live report is not a blocker-free executed certification\n";
        return 1;
    }
    if (report->environment_digest != expected_environment || report->matrix_digest != expected_matrix)
    {
        std::cerr << "FAILED: Live report is bound to an unexpected environment or matrix revision\n";
        return 1;
    }
    if (report->expires_at.empty() || report->expires_at < now)
    {
        std::cerr << "FAILED: Live report is expired\n";
        return 1;
    }
    if(report->approval_decision_id != expected_approval)
    {
        std::cerr << "FAILED: Live report is not bound to the expected approval decision\n";
        return 1;
    }
    for (const auto &cell : report->cells)
        if (!cell.executed || cell.outcome != LiveCellOutcome::Passed || cell.invocation_manifest_digest.empty() || cell.evidence_digests.empty())
        {
            std::cerr << "FAILED: Live report contains a skipped, failed, or unproven cell: " << cell.cell_id << '\n';
            return 1;
        }
    const auto digest = role_report_signing_digest(*report);
    agent_framework::approval::SQLiteApprovalStore approval_store(approval_store_path);
    StoreBackedProductionApprovalVerifier approval_verifier(
        approval_store, approval_tenant, approval_scope, now);
    std::string approval_error;
    if(!approval_verifier.verify(digest, expected_approval, &approval_error))
    {
        std::cerr << "FAILED: Live report approval is invalid: " << approval_error << '\n';
        return 1;
    }
    std::string signature_error;
    const auto public_key = read_file(public_key_path);
    if (public_key.empty() || report->signature.key_id != expected_key_id ||
        report->signature.signed_digest != digest ||
        !verify_ed25519_signature(report->signature, public_key, &signature_error))
    {
        std::cerr << "FAILED: Live report Ed25519 signature is invalid: " << signature_error << '\n';
        return 1;
    }
    std::cout << "CERTIFIED: executed=true environment=" << report->environment_digest << " matrix=" << report->matrix_digest << '\n';
    return 0;
}
