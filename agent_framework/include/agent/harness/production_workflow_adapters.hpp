#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "agent/assurance/professional_workflow.hpp"
#include "agent/eval/judge_workflow.hpp"
#include "agent/harness/workflow_adapter.hpp"
#include "agent/memory_v2/workflows/memory_workflow.hpp"
#include "agent/planning/cognition_pipeline.hpp"
#include "agent/remediation/remediation_workflow.hpp"

namespace agent_framework::harness {

struct CognitionWorkflowInput {
    planning::TaskIntake intake;
    memory_v2::MemoryScope subject;
    planning::CognitionPipelineOptions options;
};
struct MemoryWorkflowAdapterInput {
    memory_v2::workflows::MemoryWorkflowInput input;
    memory_v2::workflows::MemoryWorkflowOptions options;
};
struct AssuranceWorkflowInput {
    assurance::AcceptanceContract contract;
    memory_v2::MemoryScope subject;
    nlohmann::json task_context = nlohmann::json::object();
    nlohmann::json artifact_manifest = nlohmann::json::object();
    assurance::AssuranceWorkflowOptions options;
};
struct RemediationWorkflowInput {
    planning::ExecutionPlan current_plan;
    assurance::AcceptanceContract contract;
    assurance::AcceptanceReport report;
    assurance::AssuranceCheckpoint assurance_checkpoint;
    remediation::ImpactInventory inventory;
    memory_v2::MemoryScope subject;
    remediation::RemediationWorkflowOptions options;
};
struct JudgeWorkflowInput {
    eval::EvaluationSuite suite;
    std::shared_ptr<const eval::DatasetRegistry> datasets;
    eval::CandidateEvaluationRun baseline;
    eval::CandidateEvaluationRun candidate;
    memory_v2::MemoryScope subject;
    eval::JudgeWorkflowOptions options;
};

class ProductionWorkflowInputAssembler {
public:
    virtual ~ProductionWorkflowInputAssembler() = default;
    virtual std::optional<CognitionWorkflowInput> cognition(
        const HarnessStageRequest&, std::string*) = 0;
    virtual std::optional<MemoryWorkflowAdapterInput> memory(
        const HarnessStageRequest&, std::string*) = 0;
    virtual std::optional<AssuranceWorkflowInput> assurance(
        const HarnessStageRequest&, bool reverification, std::string*) = 0;
    virtual std::optional<RemediationWorkflowInput> remediation(
        const HarnessStageRequest&, std::string*) = 0;
    virtual std::optional<JudgeWorkflowInput> judge(
        const HarnessStageRequest&, std::string*) = 0;
};

class CallbackProductionWorkflowInputAssembler final
    : public ProductionWorkflowInputAssembler {
public:
    using Cognition = std::function<std::optional<CognitionWorkflowInput>(
        const HarnessStageRequest&, std::string*)>;
    using Memory = std::function<std::optional<MemoryWorkflowAdapterInput>(
        const HarnessStageRequest&, std::string*)>;
    using Assurance = std::function<std::optional<AssuranceWorkflowInput>(
        const HarnessStageRequest&, bool, std::string*)>;
    using Remediation = std::function<std::optional<RemediationWorkflowInput>(
        const HarnessStageRequest&, std::string*)>;
    using Judge = std::function<std::optional<JudgeWorkflowInput>(
        const HarnessStageRequest&, std::string*)>;
    Cognition cognition_fn; Memory memory_fn; Assurance assurance_fn;
    Remediation remediation_fn; Judge judge_fn;
    std::optional<CognitionWorkflowInput> cognition(const HarnessStageRequest&, std::string*) override;
    std::optional<MemoryWorkflowAdapterInput> memory(const HarnessStageRequest&, std::string*) override;
    std::optional<AssuranceWorkflowInput> assurance(const HarnessStageRequest&, bool, std::string*) override;
    std::optional<RemediationWorkflowInput> remediation(const HarnessStageRequest&, std::string*) override;
    std::optional<JudgeWorkflowInput> judge(const HarnessStageRequest&, std::string*) override;
};

class InvocationManifestResolver {
public:
    explicit InvocationManifestResolver(std::shared_ptr<llm_runtime::LLMRuntimeStore> store)
        : store_(std::move(store)) {}
    std::vector<llm_runtime::LLMInvocationManifest> resolve(
        std::string_view tenant_id, const std::vector<std::string>& invocation_ids,
        std::string* error) const;
private:
    std::shared_ptr<llm_runtime::LLMRuntimeStore> store_;
};

class CognitionWorkflowAdapter final : public TypedWorkflowAdapter {
public:
    CognitionWorkflowAdapter(planning::MultiStageCognitionWorkflow& workflow,
        ProductionWorkflowInputAssembler& inputs, InvocationManifestResolver manifests,
        std::string revision, std::string configuration_digest);
    std::string id() const override { return "phase4.cognition.workflow"; }
    WorkflowAdapterKind kind() const noexcept override { return WorkflowAdapterKind::Cognition; }
    HarnessStage stage() const noexcept override { return HarnessStage::Cognition; }
    bool side_effecting() const noexcept override { return false; }
    std::string implementation_revision() const override { return revision_; }
    std::string configuration_digest() const override { return configuration_digest_; }
    WorkflowStageExecution run(const HarnessStageRequest&) override;
private:
    planning::MultiStageCognitionWorkflow& workflow_; ProductionWorkflowInputAssembler& inputs_;
    InvocationManifestResolver manifests_; std::string revision_, configuration_digest_;
};

class MemoryWorkflowAdapter final : public TypedWorkflowAdapter {
public:
    MemoryWorkflowAdapter(memory_v2::workflows::MultiLayerMemoryWorkflow& workflow,
        ProductionWorkflowInputAssembler& inputs, InvocationManifestResolver manifests,
        std::string revision, std::string configuration_digest);
    std::string id() const override { return "phase4.memory.workflow"; }
    WorkflowAdapterKind kind() const noexcept override { return WorkflowAdapterKind::Memory; }
    HarnessStage stage() const noexcept override { return HarnessStage::MemoryUpdate; }
    bool side_effecting() const noexcept override { return true; }
    std::string implementation_revision() const override { return revision_; }
    std::string configuration_digest() const override { return configuration_digest_; }
    WorkflowStageExecution run(const HarnessStageRequest&) override;
    std::optional<WorkflowStageExecution> reconcile(const HarnessStageRequest&) override;
private:
    memory_v2::workflows::MultiLayerMemoryWorkflow& workflow_; ProductionWorkflowInputAssembler& inputs_;
    InvocationManifestResolver manifests_; std::string revision_, configuration_digest_;
};

class AssuranceWorkflowAdapter final : public TypedWorkflowAdapter {
public:
    AssuranceWorkflowAdapter(assurance::ProfessionalAssuranceWorkflow& workflow,
        ProductionWorkflowInputAssembler& inputs, InvocationManifestResolver manifests,
        bool reverification, std::string revision, std::string configuration_digest);
    std::string id() const override;
    WorkflowAdapterKind kind() const noexcept override;
    HarnessStage stage() const noexcept override;
    bool side_effecting() const noexcept override { return false; }
    std::string implementation_revision() const override { return revision_; }
    std::string configuration_digest() const override { return configuration_digest_; }
    WorkflowStageExecution run(const HarnessStageRequest&) override;
private:
    assurance::ProfessionalAssuranceWorkflow& workflow_; ProductionWorkflowInputAssembler& inputs_;
    InvocationManifestResolver manifests_; bool reverification_;
    std::string revision_, configuration_digest_;
};

class RemediationWorkflowAdapter final : public TypedWorkflowAdapter {
public:
    RemediationWorkflowAdapter(remediation::LLMRemediationWorkflow& workflow,
        ProductionWorkflowInputAssembler& inputs, InvocationManifestResolver manifests,
        std::string revision, std::string configuration_digest);
    std::string id() const override { return "phase4.remediation.workflow"; }
    WorkflowAdapterKind kind() const noexcept override { return WorkflowAdapterKind::Remediation; }
    HarnessStage stage() const noexcept override { return HarnessStage::Remediation; }
    bool side_effecting() const noexcept override { return false; }
    std::string implementation_revision() const override { return revision_; }
    std::string configuration_digest() const override { return configuration_digest_; }
    WorkflowStageExecution run(const HarnessStageRequest&) override;
private:
    remediation::LLMRemediationWorkflow& workflow_; ProductionWorkflowInputAssembler& inputs_;
    InvocationManifestResolver manifests_; std::string revision_, configuration_digest_;
};

class JudgeWorkflowAdapter final : public TypedWorkflowAdapter {
public:
    JudgeWorkflowAdapter(eval::LLMJudgeWorkflow& workflow,
        ProductionWorkflowInputAssembler& inputs, InvocationManifestResolver manifests,
        std::string revision, std::string configuration_digest);
    std::string id() const override { return "phase4.judge.workflow"; }
    WorkflowAdapterKind kind() const noexcept override { return WorkflowAdapterKind::Judge; }
    HarnessStage stage() const noexcept override { return HarnessStage::Judge; }
    bool side_effecting() const noexcept override { return false; }
    std::string implementation_revision() const override { return revision_; }
    std::string configuration_digest() const override { return configuration_digest_; }
    WorkflowStageExecution run(const HarnessStageRequest&) override;
private:
    eval::LLMJudgeWorkflow& workflow_; ProductionWorkflowInputAssembler& inputs_;
    InvocationManifestResolver manifests_; std::string revision_, configuration_digest_;
};

}  // namespace agent_framework::harness
