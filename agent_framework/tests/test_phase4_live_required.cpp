#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <openssl/crypto.h>
#include <openssl/hmac.h>

#include "agent/live/role_certification.hpp"

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
    std::string hex(const unsigned char *data, unsigned int size)
    {
        static constexpr char digits[] = "0123456789abcdef";
        std::string out;
        out.reserve(size * 2);
        for (unsigned int i = 0; i < size; ++i)
        {
            out.push_back(digits[data[i] >> 4]);
            out.push_back(digits[data[i] & 15]);
        }
        return out;
    }
    std::string hmac_sha256(std::string_view key, std::string_view value)
    {
        unsigned char bytes[EVP_MAX_MD_SIZE];
        unsigned int size = 0;
        if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), reinterpret_cast<const unsigned char *>(value.data()), value.size(), bytes, &size))
            return {};
        return hex(bytes, size);
    }
    bool equal_constant_time(std::string_view a, std::string_view b) { return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0; }
}

int main()
{
    using namespace agent_framework::live;
    const auto report_path = required("AGENT_PHASE4_LIVE_REPORT");
    const auto expected_environment = required("AGENT_PHASE4_LIVE_EXPECTED_ENVIRONMENT_DIGEST");
    const auto expected_matrix = required("AGENT_PHASE4_LIVE_EXPECTED_MATRIX_DIGEST");
    const auto expected_key_id = required("AGENT_PHASE4_LIVE_SIGNING_KEY_ID");
    const auto signing_key = required("AGENT_PHASE4_LIVE_REPORT_SIGNING_KEY");
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
    for (const auto &cell : report->cells)
        if (!cell.executed || cell.outcome != LiveCellOutcome::Passed || cell.invocation_manifest_digest.empty() || cell.evidence_digests.empty())
        {
            std::cerr << "FAILED: Live report contains a skipped, failed, or unproven cell: " << cell.cell_id << '\n';
            return 1;
        }
    const auto digest = role_report_signing_digest(*report);
    if (report->signature.algorithm != "hmac-sha256" || report->signature.key_id != expected_key_id || report->signature.signed_digest != digest || !equal_constant_time(report->signature.signature, hmac_sha256(signing_key, digest)))
    {
        std::cerr << "FAILED: Live report signature is invalid\n";
        return 1;
    }
    std::cout << "CERTIFIED: executed=true environment=" << report->environment_digest << " matrix=" << report->matrix_digest << '\n';
    return 0;
}
