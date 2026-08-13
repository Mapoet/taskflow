#pragma once

#include <functional>
#include <optional>

#include "agent/agent_template/planning.hpp"
#include "agent/harness/task_closure.hpp"

namespace agent_framework::agent_template {

enum class ReplanTriggerKind {
    SkillUnavailable,
    PermissionDenied,
    ApprovalRejected,
    ToolFailed,
    EvidenceInsufficient,
    VerificationFailed,
    BudgetThresholdReached,
    UserChangedRequirement,
    StagnationDetected
};

struct ReplanTrigger {
    ReplanTriggerKind kind{ReplanTriggerKind::ToolFailed};
    std::string source_node_id;
    std::string reason;
    nlohmann::json observations = nlohmann::json::object();
    std::uint64_t expected_current_revision{0};
};

struct ReplanResult {
    bool ok{false};
    std::optional<SkillCollaborationPlan> plan;
    std::vector<contracts::ContractIssue> issues;
    std::string error_code;
};

class ReplanCoordinator {
public:
    ReplanResult replan(SkillPlanProvider& provider,
                        const PlanningContext& context,
                        const AgentTemplate& agent_template,
                        const SkillCollaborationPlan& current,
                        const ReplanTrigger& trigger,
                        const std::vector<SkillCandidate>& candidates) const;
};

struct CompletionEvidence {
    AgentTemplateInvocation invocation;
    SkillCollaborationPlan plan;
    ActiveSkillSession session;
    std::vector<SkillRunnerReceipt> receipts;
    nlohmann::json output = nlohmann::json::object();
};

struct AgentCompletionDecision {
    bool accepted{false};
    std::string authority;
    std::string reason_code;
    std::string decision_digest;
    std::vector<std::string> evidence_refs;
    std::vector<std::string> limitations;
};

class AgentCompletionAuthority {
public:
    virtual ~AgentCompletionAuthority() = default;
    virtual AgentCompletionDecision evaluate(const CompletionEvidence&) = 0;
};

using CompletionCallback = std::function<AgentCompletionDecision(const CompletionEvidence&)>;
class CallbackCompletionAuthority final : public AgentCompletionAuthority {
public:
    explicit CallbackCompletionAuthority(CompletionCallback callback)
        : callback_(std::move(callback)) {}
    AgentCompletionDecision evaluate(const CompletionEvidence&) override;
private:
    CompletionCallback callback_;
};

class TaskClosureCompletionAuthority final : public AgentCompletionAuthority {
public:
    using FactsAssembler = std::function<harness::ClosureFacts(const CompletionEvidence&)>;
    TaskClosureCompletionAuthority(harness::TaskClosureContract contract,
                                   FactsAssembler assembler,
                                   harness::TaskClosureController controller = {});
    AgentCompletionDecision evaluate(const CompletionEvidence&) override;
private:
    harness::TaskClosureContract contract_;
    FactsAssembler assembler_;
    harness::TaskClosureController controller_;
};

std::string to_string(ReplanTriggerKind value);

}  // namespace agent_framework::agent_template
