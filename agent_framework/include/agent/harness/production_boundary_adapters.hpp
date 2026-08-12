#pragma once

#include <functional>

#include "agent/execution/harness_ports.hpp"
#include "agent/harness/production_workflow_adapters.hpp"
#include "agent/ui/store_backed_operations.hpp"

namespace agent_framework::harness {

class StoreBackedIntakeAdapter final : public TypedWorkflowAdapter {
public:
    StoreBackedIntakeAdapter(ProductionWorkflowInputRepository& repository,
                             std::string revision, std::string configuration_digest);
    std::string id() const override { return "phase4.intake.store"; }
    WorkflowAdapterKind kind() const noexcept override { return WorkflowAdapterKind::Intake; }
    HarnessStage stage() const noexcept override { return HarnessStage::Intake; }
    bool side_effecting() const noexcept override { return false; }
    std::string implementation_revision() const override { return revision_; }
    std::string configuration_digest() const override { return configuration_digest_; }
    WorkflowStageExecution run(const HarnessStageRequest&) override;
private:
    ProductionWorkflowInputRepository& repository_;
    std::string revision_, configuration_digest_;
};

class StoreBackedApprovalAdapter final : public TypedWorkflowAdapter {
public:
    StoreBackedApprovalAdapter(approval::ApprovalStore& store,
                               std::function<std::string()> now,
                               std::string revision, std::string configuration_digest);
    std::string id() const override { return "phase4.approval.store"; }
    WorkflowAdapterKind kind() const noexcept override { return WorkflowAdapterKind::Approval; }
    HarnessStage stage() const noexcept override { return HarnessStage::PlanApproval; }
    bool side_effecting() const noexcept override { return false; }
    std::string implementation_revision() const override { return revision_; }
    std::string configuration_digest() const override { return configuration_digest_; }
    WorkflowStageExecution run(const HarnessStageRequest&) override;
private:
    approval::ApprovalStore& store_;
    std::function<std::string()> now_;
    std::string revision_, configuration_digest_;
};

class ArtifactExecutionWorkflowAdapter final : public TypedWorkflowAdapter {
public:
    ArtifactExecutionWorkflowAdapter(execution::WorkspaceArtifactExecutor& executor,
        execution::ArtifactAction action, execution::ArtifactJournal& journal,
        std::string revision, std::string configuration_digest,
        std::string parent_idempotency_key = {});
    std::string id() const override { return "phase4.execution.artifact"; }
    WorkflowAdapterKind kind() const noexcept override { return WorkflowAdapterKind::Execution; }
    HarnessStage stage() const noexcept override { return HarnessStage::Execution; }
    bool side_effecting() const noexcept override { return true; }
    std::string implementation_revision() const override { return revision_; }
    std::string configuration_digest() const override { return configuration_digest_; }
    WorkflowStageExecution run(const HarnessStageRequest&) override;
    std::optional<WorkflowStageExecution> reconcile(const HarnessStageRequest&) override;
private:
    execution::ArtifactExecutionHarnessPort port_;
    std::string revision_, configuration_digest_;
};

class StoreBackedOperationsAdapter final : public TypedWorkflowAdapter {
public:
    StoreBackedOperationsAdapter(StoreBackedOperationsAssembler& assembler,
        memory_v2::MemoryScope subject, std::function<std::string()> now,
        std::string revision, std::string configuration_digest);
    std::string id() const override { return "phase4.operations.store"; }
    WorkflowAdapterKind kind() const noexcept override { return WorkflowAdapterKind::Operations; }
    HarnessStage stage() const noexcept override { return HarnessStage::Operations; }
    bool side_effecting() const noexcept override { return true; }
    std::string implementation_revision() const override { return revision_; }
    std::string configuration_digest() const override { return configuration_digest_; }
    WorkflowStageExecution run(const HarnessStageRequest&) override;
    std::optional<WorkflowStageExecution> reconcile(const HarnessStageRequest&) override;
private:
    WorkflowStageExecution invoke(const HarnessStageRequest&);
    StoreBackedOperationsAssembler& assembler_;
    memory_v2::MemoryScope subject_;
    std::function<std::string()> now_;
    std::string revision_, configuration_digest_;
};

}  // namespace agent_framework::harness
