#ifndef AGENT_SKILL_RUNTIME_HPP
#define AGENT_SKILL_RUNTIME_HPP

#include "skill_loader.hpp"
#include "skill_policy.hpp"
#include "task_state_machine.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace agent_framework {

inline constexpr const char* kSkillPermissionDenied = "skill_permission_denied";
inline constexpr const char* kSkillInputInvalid = "skill_input_invalid";
inline constexpr const char* kSkillOutputInvalid = "skill_output_invalid";
inline constexpr const char* kSkillResourceBudgetExceeded = "skill_resource_budget_exceeded";
inline constexpr const char* kSkillCancelled = "skill_cancelled";
inline constexpr const char* kSkillDependencyUnavailable = "skill_dependency_unavailable";

enum class SkillEventType {
    InvocationStarted,
    InvocationCompleted,
    PermissionDenied,
    InputInvalid,
    OutputInvalid,
    BudgetExceeded,
    Cancelled,
    TimedOut
};

struct SkillEvent {
    SkillEventType type = SkillEventType::InvocationStarted;
    std::string code;
    std::string skill_id;
    std::string resource_id;
    std::string task_id;
    std::string run_id;
    int attempt = 0;
    int iteration = 0;
    nlohmann::json details = nlohmann::json::object();
};

using SkillEventSink = std::function<void(const SkillEvent&)>;

struct SkillRuntimeLimits {
    std::size_t max_input_bytes = 1024U * 1024U;
    std::size_t max_output_bytes = 1024U * 1024U;
};

struct SkillInvocationContext {
    std::shared_ptr<TaskControl> control;
    SkillPermissionGrant grants;
    SkillEventSink event_sink;
    SkillRuntimeLimits limits;
    /** Values are request data and are never copied into events or normalized manifests. */
    std::map<std::string, std::string> environment;
    std::function<std::optional<std::string>(std::string_view)> secret_provider;
    std::string task_id;
    std::string run_id;
    int attempt = 0;
    int iteration = 0;
};

struct SkillInvocationTicket {
    std::string skill_id;
    std::shared_ptr<const SkillManifest> manifest;
    SkillResourceDescriptor resource;
    std::shared_ptr<const SkillPolicyEngine> policy;
    SkillInvocationContext context;
};

struct SkillRuntimeResult {
    bool ok = false;
    nlohmann::json error = nlohmann::json::object();
    std::optional<SkillInvocationTicket> ticket;
};

class SkillRuntime {
public:
    SkillRuntime(std::shared_ptr<SkillRegistry> registry, std::shared_ptr<SkillLoader> loader);

    SkillRuntimeResult begin(const std::string& skill_id, const std::string& resource_id,
                             SkillResourceType expected_kind, const nlohmann::json& input,
                             SkillInvocationContext context) const;
    SkillRuntimeResult finish(const SkillInvocationTicket& ticket,
                              const nlohmann::json& output) const;
    void record_termination(const SkillInvocationTicket& ticket, bool timed_out) const noexcept;

    static nlohmann::json permission_error(const SkillPolicyDecision& decision);

private:
    std::shared_ptr<SkillRegistry> registry_;
    std::shared_ptr<SkillLoader> loader_;

    SkillRuntimeResult validate_schema(const SkillInvocationTicket& ticket,
                                       const std::string& schema_id,
                                       const nlohmann::json& value, bool input) const;
};

const char* skill_event_type_cstr(SkillEventType type) noexcept;

} // namespace agent_framework

#endif
