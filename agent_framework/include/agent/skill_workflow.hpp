#ifndef AGENT_SKILL_WORKFLOW_HPP
#define AGENT_SKILL_WORKFLOW_HPP

#include "child_task.hpp"
#include "skill_capability_runtime.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>

namespace agent_framework {

inline constexpr const char* kSkillWorkflowDescriptorInvalid =
    "skill_workflow_descriptor_invalid";
inline constexpr const char* kSkillWorkflowMappingInvalid =
    "skill_workflow_mapping_invalid";
inline constexpr const char* kSkillWorkflowDependencyMismatch =
    "skill_workflow_dependency_mismatch";
inline constexpr const char* kSkillWorkflowCheckpointIncompatible =
    "skill_workflow_checkpoint_incompatible";
inline constexpr const char* kSkillWorkflowReplayDenied =
    "skill_workflow_replay_denied";
inline constexpr const char* kSkillWorkflowChildFailed =
    "skill_workflow_child_failed";

enum class SkillWorkflowStartMode { Start, Retry, Restart, Resume };

struct SkillWorkflowRunOptions {
    SkillWorkflowStartMode mode = SkillWorkflowStartMode::Start;
    SkillInvocationContext context;
    nlohmann::json checkpoint = nlohmann::json::object();
    std::size_t max_depth = 8;
    std::function<std::shared_ptr<ChildTaskBackend>(const std::string&)>
        child_backend;
};

struct SkillWorkflowResult {
    bool ok = false;
    nlohmann::json output = nlohmann::json::object();
    nlohmann::json error = nlohmann::json::object();
    nlohmann::json checkpoint = nlohmann::json::object();
    nlohmann::json events = nlohmann::json::array();
    std::map<std::string, std::string> dependency_lock;
};

struct SkillWorkflowValidationResult {
    bool ok = false;
    nlohmann::json error = nlohmann::json::object();
};

SkillWorkflowValidationResult validate_skill_workflow_descriptor(
    const nlohmann::json& descriptor);

class SkillWorkflowRuntime {
public:
    SkillWorkflowRuntime(std::shared_ptr<SkillRegistry> registry,
                         std::shared_ptr<SkillLoader> loader,
                         std::shared_ptr<SkillRuntime> skill_runtime,
                         std::shared_ptr<SkillCapabilityRuntime> capabilities);

    SkillWorkflowResult run(const std::string& skill_id,
                            const std::string& workflow_resource_id,
                            const nlohmann::json& input,
                            SkillWorkflowRunOptions options = {}) const;

private:
    std::shared_ptr<SkillRegistry> registry_;
    std::shared_ptr<SkillLoader> loader_;
    std::shared_ptr<SkillRuntime> skill_runtime_;
    std::shared_ptr<SkillCapabilityRuntime> capabilities_;
};

const char* skill_workflow_start_mode_cstr(SkillWorkflowStartMode mode) noexcept;

} // namespace agent_framework

#endif
