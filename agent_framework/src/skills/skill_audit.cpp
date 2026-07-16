#include <agent/skills/skill_audit.hpp>

namespace agent_framework {

nlohmann::json SkillAuditRecord::to_json() const {
    return {{"identity",{{"skillId",identity.skill_id},{"skillVersion",identity.skill_version},
                         {"packageDigest",identity.package_digest},
                         {"registryGeneration",identity.registry_generation},
                         {"taskId",identity.task_id},{"sessionId",identity.session_id},
                         {"traceId",identity.trace_id},{"runId",identity.run_id},
                         {"attempt",identity.attempt},{"iteration",identity.iteration},
                         {"depth",identity.depth}}},
            {"category",category},{"action",action},{"resourceId",resource_id},
            {"outcome",outcome},{"code",code},{"details",details}};
}

void emit_skill_audit(const SkillAuditSink& sink, SkillAuditRecord record) noexcept {
    if(!sink) return;
    try { sink(record); } catch(...) {}
}

} // namespace agent_framework
