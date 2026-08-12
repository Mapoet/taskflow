#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "agent/harness/runtime.hpp"
#include "agent/llm_runtime/types.hpp"
#include "agent/telemetry/runtime.hpp"

namespace agent_framework::harness {

enum class WorkflowAdapterKind {
    Intake, Cognition, Approval, Execution, Memory, Assurance,
    Remediation, Reverification, Judge, Operations
};

struct WorkflowStageExecution {
    HarnessStageResult result;
    std::vector<llm_runtime::LLMInvocationManifest> invocations;
};

class TypedWorkflowAdapter {
public:
    virtual ~TypedWorkflowAdapter() = default;
    virtual std::string id() const = 0;
    virtual WorkflowAdapterKind kind() const noexcept = 0;
    virtual HarnessStage stage() const noexcept = 0;
    virtual bool side_effecting() const noexcept = 0;
    virtual std::string implementation_revision() const = 0;
    virtual std::string configuration_digest() const = 0;
    virtual WorkflowStageExecution run(const HarnessStageRequest& request) = 0;
    virtual std::optional<WorkflowStageExecution> reconcile(
        const HarnessStageRequest&) { return std::nullopt; }
};

class LLMInvocationObserver {
public:
    virtual ~LLMInvocationObserver() = default;
    virtual bool observe(const HarnessStageRequest& request,
                         const llm_runtime::LLMInvocationManifest& manifest,
                         std::string* error = nullptr) = 0;
};

class TelemetryLLMInvocationObserver final : public LLMInvocationObserver {
public:
    explicit TelemetryLLMInvocationObserver(telemetry::TelemetryRuntime& telemetry)
        : telemetry_(telemetry) {}
    bool observe(const HarnessStageRequest& request,
                 const llm_runtime::LLMInvocationManifest& manifest,
                 std::string* error = nullptr) override;
private:
    telemetry::TelemetryRuntime& telemetry_;
};

class WorkflowHarnessStagePort final : public HarnessStagePort {
public:
    WorkflowHarnessStagePort(std::shared_ptr<TypedWorkflowAdapter> adapter,
                             std::shared_ptr<LLMInvocationObserver> observer);
    std::string id() const override;
    bool may_have_side_effects() const noexcept override;
    bool production_ready() const noexcept override;
    std::string capability_manifest_digest() const override;
    HarnessStageResult execute(const HarnessStageRequest& request) override;
    std::optional<HarnessStageResult> reconcile(
        const HarnessStageRequest& request) override;
private:
    HarnessStageResult finalize(const HarnessStageRequest& request,
                                WorkflowStageExecution execution);
    std::shared_ptr<TypedWorkflowAdapter> adapter_;
    std::shared_ptr<LLMInvocationObserver> observer_;
    std::string manifest_digest_;
};

}  // namespace agent_framework::harness
