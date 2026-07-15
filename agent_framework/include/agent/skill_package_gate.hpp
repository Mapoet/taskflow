#ifndef AGENT_SKILL_PACKAGE_GATE_HPP
#define AGENT_SKILL_PACKAGE_GATE_HPP

#include <agent/skill_lifecycle.hpp>
#include <agent/skill_test_runner.hpp>

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
};

} // namespace agent_framework

#endif
