#ifndef AGENT_SKILL_PACKAGE_GATE_HPP
#define AGENT_SKILL_PACKAGE_GATE_HPP

#include <agent/skills/skill_lifecycle.hpp>
#include <agent/skills/skill_sbom.hpp>
#include <agent/skills/skill_test_runner.hpp>

namespace agent_framework {

struct SkillPackageGateResult {
    bool ok = false;
    nlohmann::json error = nullptr;
    std::optional<SkillPackageRecord> package;
    std::vector<SkillDiagnostic> diagnostics;
    std::optional<SkillTestSuiteResult> tests;

    nlohmann::json to_json() const;
};

class SkillPackageGate {
public:
    SkillPackageGateResult inspect(const std::filesystem::path& package) const;

    struct TrustOptions {
        SkillTrustStore trust;
        std::optional<SkillSignatureEnvelope> signature;
        bool remote = false;
        bool allow_unsigned_local = false;
        std::int64_t now = 0;
    };

    SkillPackageGateResult inspect_archive(const std::filesystem::path& archive,
                                           const TrustOptions& options) const;
};

} // namespace agent_framework

#endif
