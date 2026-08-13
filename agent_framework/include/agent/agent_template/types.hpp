#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/contracts/contract.hpp"
#include "agent/skills/skill_policy.hpp"

namespace agent_framework::agent_template
{

    inline constexpr std::string_view kAgentTemplateKind = "agent.agent_template/v1";
    inline constexpr std::string_view kCollaborationPlanKind =
        "agent.skill_collaboration_plan/v1";
    inline constexpr std::string_view kActiveSkillSessionKind =
        "agent.active_skill_session/v1";
    inline constexpr std::string_view kTemplateInvocationKind =
        "agent.agent_template_invocation/v1";

    enum class AgentBusinessMode
    {
        ModelDriven,
        DirectiveDriven,
        Hybrid,
        FixedWorkflow
    };
    enum class AgentHostingMode
    {
        Standalone,
        Conversation,
        WorkflowNode,
        WorkflowSubflow,
        RemoteA2A
    };
    enum class SkillRole
    {
        Coordinator,
        Worker,
        Synthesizer,
        Verifier,
        Judge,
        Approver
    };
    enum class SkillRunnerKind
    {
        InlinePrompt,
        LocalCapability,
        SandboxedProcess,
        Cli,
        Mcp,
        ChildAgent,
        NestedWorkflow,
        HumanApproval
    };
    enum class EffectClass
    {
        ReadOnly,
        Write,
        Unknown
    };
    enum class SessionState
    {
        Prepared,
        Active,
        Suspended,
        Completed,
        Failed,
        Cancelled
    };
    enum class RunnerLifecycleState
    {
        Admitted,
        Prepared,
        Running,
        Attached,
        Waiting,
        Checkpointed,
        Cancelling,
        Reconciling,
        Succeeded,
        Failed,
        Cancelled
    };

    struct TemplateRef
    {
        std::string template_id;
        std::uint64_t revision{0};
        std::string digest;
    };

    struct TypedRef
    {
        std::string id;
        std::string kind;
        std::string digest;
        std::string uri;
    };
    using ArtifactRef = TypedRef;
    using EvidenceRef = TypedRef;
    using ReceiptRef = TypedRef;

    struct PermissionEnvelope
    {
        std::vector<std::string> tools;
        std::vector<std::string> network;
        std::vector<std::string> filesystem_read;
        std::vector<std::string> filesystem_write;
        std::vector<std::string> environment;
        std::vector<std::string> secrets;
    };

    struct BudgetPolicy
    {
        std::uint64_t max_iterations{32};
        std::uint64_t max_tool_calls{128};
        std::uint64_t max_parallelism{8};
        std::uint64_t max_tokens{0};
        std::uint64_t deadline_ms{0};
        double max_cost{0.0};
    };

    struct SkillSelector
    {
        std::optional<std::string> skill_id;
        std::vector<std::string> required_capabilities;
        std::vector<std::string> candidates;
        bool choose_by_model{false};
    };

    struct OutputContract
    {
        nlohmann::json schema = nlohmann::json::object();
        bool artifact_required{false};
        bool evidence_required{false};
    };

    struct SkillPlanNode
    {
        std::string node_id;
        SkillRole role{SkillRole::Worker};
        SkillSelector selector;
        std::optional<std::string> resolved_skill_id;
        std::string resolved_skill_version;
        std::string resolved_skill_digest;
        SkillRunnerKind runner{SkillRunnerKind::InlinePrompt};
        nlohmann::json input_mapping = nlohmann::json::object();
        OutputContract output;
        PermissionEnvelope requested_permissions;
        EffectClass effect{EffectClass::ReadOnly};
        std::uint64_t max_attempts{1};
        std::string failure_policy{"fail"};
        std::string idempotency_key;
        bool required{true};
        bool verifier{false};
        bool approval_required{false};
        bool model_replannable{false};
    };

    struct SkillPlanEdge
    {
        std::string from;
        std::string to;
        std::string condition;
    };

    struct SkillCollaborationPlan
    {
        contracts::ContractMetadata metadata;
        std::string plan_id;
        std::uint64_t revision{1};
        std::string parent_digest;
        std::vector<SkillPlanNode> nodes;
        std::vector<SkillPlanEdge> edges;
        BudgetPolicy budget;
        nlohmann::json output_assembly = nlohmann::json::object();
        std::vector<std::string> committed_effect_receipts;
    };

    struct AgentRoleDefinition
    {
        std::string role_id;
        SkillRole role{SkillRole::Worker};
        SkillSelector selector;
        std::uint64_t min_cardinality{1};
        std::uint64_t max_cardinality{1};
        bool required{true};
    };

    struct AgentTemplate
    {
        contracts::ContractMetadata metadata;
        std::string template_id;
        std::uint64_t revision{1};
        std::string name;
        AgentBusinessMode business_mode{AgentBusinessMode::Hybrid};
        nlohmann::json input_schema = nlohmann::json::object();
        nlohmann::json output_schema = nlohmann::json::object();
        PermissionEnvelope permissions;
        BudgetPolicy budgets;
        std::vector<AgentRoleDefinition> roles;
        std::optional<SkillCollaborationPlan> workflow_skeleton;
        nlohmann::json planning_policy = nlohmann::json::object();
        nlohmann::json recovery_policy = nlohmann::json::object();
        nlohmann::json approval_policy = nlohmann::json::object();
        nlohmann::json assurance_policy = nlohmann::json::object();
        nlohmann::json completion_contract = nlohmann::json::object();
    };

    struct PinnedSkill
    {
        std::string skill_id;
        std::string version;
        std::string package_digest;
        std::map<std::string, std::string> dependency_lock;
        PermissionEnvelope effective_permissions;
        std::string capability_snapshot_digest;
    };

    struct ActiveSkillSession
    {
        contracts::ContractMetadata metadata;
        std::string session_id;
        std::uint64_t revision{1};
        SessionState state{SessionState::Prepared};
        std::string plan_digest;
        std::string registry_generation;
        std::vector<PinnedSkill> skills;
        PermissionEnvelope effective_permissions;
        BudgetPolicy budget;
        std::string model_profiles_digest;
        std::string prompt_revisions_digest;
        std::string capability_snapshot_digest;
        std::string deployment_generation;
        std::string checkpoint_ref;
    };

    struct AgentTemplateInvocation
    {
        contracts::ContractMetadata metadata;
        std::string invocation_id;
        std::uint64_t revision{1};
        TemplateRef template_ref;
        std::string plan_digest;
        std::string skill_session_digest;
        std::string model_profiles_digest;
        std::string capability_snapshot_digest;
        PermissionEnvelope permissions;
        BudgetPolicy budget;
        std::string context_projection_ref;
        std::string deployment_generation;
        AgentBusinessMode business_mode{AgentBusinessMode::Hybrid};
        AgentHostingMode hosting_mode{AgentHostingMode::Standalone};
    };

    struct RunnerEvent
    {
        std::string event_id;
        std::uint64_t sequence{0};
        std::string node_id;
        RunnerLifecycleState state{RunnerLifecycleState::Admitted};
        std::string timestamp;
        nlohmann::json details = nlohmann::json::object();
    };

    struct SkillRunnerReceipt
    {
        std::string invocation_id;
        std::string node_id;
        SkillRunnerKind runner{SkillRunnerKind::InlinePrompt};
        RunnerLifecycleState terminal_state{RunnerLifecycleState::Failed};
        std::vector<ArtifactRef> artifacts;
        std::vector<EvidenceRef> evidence;
        std::vector<ReceiptRef> effects;
        std::string checkpoint_ref;
        std::string output_digest;
    };

    std::string to_string(AgentBusinessMode value);
    std::string to_string(AgentHostingMode value);
    std::string to_string(SkillRole value);
    std::string to_string(SkillRunnerKind value);
    std::string to_string(EffectClass value);
    std::string to_string(SessionState value);
    std::string to_string(RunnerLifecycleState value);

    std::optional<AgentBusinessMode> agent_business_mode_from_string(std::string_view value);
    std::optional<AgentHostingMode> agent_hosting_mode_from_string(std::string_view value);
    std::optional<SkillRole> skill_role_from_string(std::string_view value);
    std::optional<SkillRunnerKind> skill_runner_kind_from_string(std::string_view value);
    std::optional<EffectClass> effect_class_from_string(std::string_view value);
    std::optional<SessionState> session_state_from_string(std::string_view value);
    std::optional<RunnerLifecycleState> runner_state_from_string(std::string_view value);

    nlohmann::json encode(const AgentTemplate &value);
    nlohmann::json encode(const SkillCollaborationPlan &value);
    nlohmann::json encode(const ActiveSkillSession &value);
    nlohmann::json encode(const AgentTemplateInvocation &value);
    nlohmann::json encode(const SkillRunnerReceipt &value);

    std::optional<AgentTemplate> decode_agent_template(
        const nlohmann::json &value, const contracts::ParseContext &context = {},
        std::vector<contracts::ContractIssue> *issues = nullptr);
    std::optional<SkillCollaborationPlan> decode_collaboration_plan(
        const nlohmann::json &value, const contracts::ParseContext &context = {},
        std::vector<contracts::ContractIssue> *issues = nullptr);
    std::optional<ActiveSkillSession> decode_active_skill_session(
        const nlohmann::json &value, const contracts::ParseContext &context = {},
        std::vector<contracts::ContractIssue> *issues = nullptr);
    std::optional<AgentTemplateInvocation> decode_template_invocation(
        const nlohmann::json &value, const contracts::ParseContext &context = {},
        std::vector<contracts::ContractIssue> *issues = nullptr);

} // namespace agent_framework::agent_template
