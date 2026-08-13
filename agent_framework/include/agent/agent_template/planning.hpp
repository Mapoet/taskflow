#pragma once

#include <functional>
#include <memory>
#include <optional>

#include "agent/agent_template/session.hpp"

namespace agent_framework::agent_template
{

    struct PlanningContext
    {
        contracts::ContractMetadata metadata;
        AgentTemplate agent_template;
        nlohmann::json input = nlohmann::json::object();
        PermissionEnvelope permissions;
        BudgetPolicy budget;
        std::uint64_t expected_revision{0};
    };
    struct PlanValidationResult
    {
        bool ok{false};
        std::vector<contracts::ContractIssue> issues;
    };

    class SkillPlanProvider
    {
    public:
        virtual ~SkillPlanProvider() = default;
        virtual SkillCollaborationPlan propose(const PlanningContext &context,
                                               const std::vector<SkillCandidate> &candidates) = 0;
        virtual SkillCollaborationPlan revise(const PlanningContext &context,
                                              const SkillCollaborationPlan &current,
                                              const nlohmann::json &observations,
                                              const std::vector<SkillCandidate> &candidates) = 0;
    };
    class FixedWorkflowPlanProvider final : public SkillPlanProvider
    {
    public:
        SkillCollaborationPlan propose(const PlanningContext &, const std::vector<SkillCandidate> &) override;
        SkillCollaborationPlan revise(const PlanningContext &, const SkillCollaborationPlan &,
                                      const nlohmann::json &, const std::vector<SkillCandidate> &) override;
    };
    class DirectiveSkillPlanProvider final : public SkillPlanProvider
    {
    public:
        SkillCollaborationPlan propose(const PlanningContext &, const std::vector<SkillCandidate> &) override;
        SkillCollaborationPlan revise(const PlanningContext &, const SkillCollaborationPlan &,
                                      const nlohmann::json &, const std::vector<SkillCandidate> &) override;
    };
    using ModelPlanCallback = std::function<SkillCollaborationPlan(
        const PlanningContext &, const std::optional<SkillCollaborationPlan> &,
        const nlohmann::json &, const std::vector<SkillCandidate> &)>;
    class ModelSkillPlanProvider final : public SkillPlanProvider
    {
    public:
        explicit ModelSkillPlanProvider(ModelPlanCallback callback) : callback_(std::move(callback)) {}
        SkillCollaborationPlan propose(const PlanningContext &, const std::vector<SkillCandidate> &) override;
        SkillCollaborationPlan revise(const PlanningContext &, const SkillCollaborationPlan &,
                                      const nlohmann::json &, const std::vector<SkillCandidate> &) override;

    private:
        ModelPlanCallback callback_;
    };
    class HybridSkillPlanProvider final : public SkillPlanProvider
    {
    public:
        explicit HybridSkillPlanProvider(ModelPlanCallback callback) : model_(std::move(callback)) {}
        SkillCollaborationPlan propose(const PlanningContext &, const std::vector<SkillCandidate> &) override;
        SkillCollaborationPlan revise(const PlanningContext &, const SkillCollaborationPlan &,
                                      const nlohmann::json &, const std::vector<SkillCandidate> &) override;

    private:
        ModelSkillPlanProvider model_;
    };

    class SkillCollaborationPlanValidator
    {
    public:
        PlanValidationResult validate(const AgentTemplate &agent_template,
                                      const SkillCollaborationPlan &candidate,
                                      const PermissionEnvelope &permission_ceiling,
                                      const BudgetPolicy &budget_ceiling,
                                      const SkillCollaborationPlan *prior = nullptr,
                                      std::uint64_t expected_revision = 0) const;
    };
} // namespace agent_framework::agent_template
