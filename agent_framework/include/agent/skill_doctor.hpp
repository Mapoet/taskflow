#ifndef AGENT_SKILL_DOCTOR_HPP
#define AGENT_SKILL_DOCTOR_HPP

#include <agent/skill_policy.hpp>
#include <agent/skill_registry.hpp>
#include <agent/skill_resource_cache.hpp>

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

namespace agent_framework {

struct SkillDoctorOptions {
    std::vector<std::string> available_runtimes;
    std::vector<std::string> available_models;
    std::vector<std::string> available_devices;
    std::vector<std::string> available_precisions;
    std::vector<std::string> available_executables;
    std::uint64_t available_memory_bytes = 0;
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
    explicit SkillDoctor(std::shared_ptr<SkillRegistry> registry,
                         std::shared_ptr<SkillResourceCache> cache = nullptr)
        : registry_(std::move(registry)), cache_(std::move(cache)) {}

    SkillDoctorReport inspect(const std::string& skill_id,
                              const SkillDoctorOptions& options = {}) const;

private:
    std::shared_ptr<SkillRegistry> registry_;
    std::shared_ptr<SkillResourceCache> cache_;
};

} // namespace agent_framework

#endif
