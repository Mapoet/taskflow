#ifndef AGENT_SKILL_AUDIT_HPP
#define AGENT_SKILL_AUDIT_HPP

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <string>

namespace agent_framework {

struct SkillAuditIdentity {
    std::string skill_id;
    std::string skill_version;
    std::string package_digest;
    std::uint64_t registry_generation = 0;
    std::string task_id;
    std::string session_id;
    std::string trace_id;
    std::string run_id;
    int attempt = 0;
    int iteration = 0;
    int depth = 0;
};

struct SkillAuditRecord {
    SkillAuditIdentity identity;
    std::string category;
    std::string action;
    std::string resource_id;
    std::string outcome;
    std::string code;
    nlohmann::json details = nlohmann::json::object();

    nlohmann::json to_json() const;
};

using SkillAuditSink = std::function<void(const SkillAuditRecord&)>;

void emit_skill_audit(const SkillAuditSink& sink, SkillAuditRecord record) noexcept;

} // namespace agent_framework

#endif
