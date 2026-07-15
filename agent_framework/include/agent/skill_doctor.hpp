#ifndef AGENT_SKILL_DOCTOR_HPP
#define AGENT_SKILL_DOCTOR_HPP

#include <agent/skill_policy.hpp>
#include <agent/skill_registry.hpp>

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

namespace agent_framework {

struct SkillDoctorOptions {
    std::vector<std::string> available_runtimes;
    std::vector<std::string> available_models;
    SkillPermissionGrant grants;
};

struct SkillDoctorReport {
    bool ready = false;
    std::vector<SkillDiagnostic> diagnostics;
    nlohmann::json checks = nlohmann::json::object();

    nlohmann::json to_json() const;
};

class SkillDoctor {
public:
    explicit SkillDoctor(std::shared_ptr<SkillRegistry> registry)
        : registry_(std::move(registry)) {}

    SkillDoctorReport inspect(const std::string& skill_id,
                              const SkillDoctorOptions& options = {}) const;

private:
    std::shared_ptr<SkillRegistry> registry_;
};

} // namespace agent_framework

#endif
