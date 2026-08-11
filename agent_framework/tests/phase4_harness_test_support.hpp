#pragma once

#include <map>
#include <memory>
#include <string>

#include "agent/harness/runtime.hpp"

namespace phase4_harness_test {
using namespace agent_framework;

inline contracts::ContractMetadata metadata(std::string run = "run-harness") {
    contracts::ContractMetadata value;
    value.identity.tenant_id = "tenant-a";
    value.identity.organization_id = "org-a";
    value.identity.principal_id = "principal-a";
    value.identity.project_id = "project-a";
    value.identity.task_id = "task-a";
    value.identity.run_id = std::move(run);
    value.identity.plan_id = "plan-a";
    return value;
}

inline harness::HarnessStart start(std::string id = "harness-a") {
    harness::HarnessStart value;
    value.metadata = metadata();
    value.harness_id = std::move(id);
    value.intake_digest = "sha256:intake";
    value.acceptance_contract_digest = "sha256:acceptance-contract";
    value.profile_revision_digest = "sha256:profiles";
    value.prompt_revision_digest = "sha256:prompts";
    value.max_remediation_cycles = 2;
    value.judge_required = true;
    return value;
}

struct PortCounters {
    std::map<harness::HarnessStage, std::uint64_t> execute;
    std::map<harness::HarnessStage, std::uint64_t> reconcile;
};

inline harness::HarnessStageResult successful(
    const harness::HarnessStageRequest& request, bool remediation_path = true) {
    using namespace harness;
    HarnessStageResult result;
    result.outcome = StageOutcome::Succeeded;
    result.output_digest = "sha256:output:" + harness_stage_name(request.stage) + ":" +
                           std::to_string(request.attempt);
    result.invocation_manifest_digest =
        "sha256:manifest:" + harness_stage_name(request.stage);
    switch(request.stage) {
        case HarnessStage::Cognition:
            result.pins.plan_digest = "sha256:plan-v1";
            break;
        case HarnessStage::PlanApproval:
            result.pins.approval_decision_id = "approval-plan-v1";
            break;
        case HarnessStage::Execution:
            result.pins.artifact_manifest_digest = "sha256:artifact-v1";
            result.effect_receipt_digest = "sha256:effect-execution-v1";
            break;
        case HarnessStage::MemoryUpdate:
            result.pins.memory_snapshot_id = "memory-snapshot-v1";
            result.pins.memory_view_digest = "sha256:memory-view-v1";
            break;
        case HarnessStage::Assurance:
            result.pins.acceptance_report_digest = "sha256:acceptance-report-failed";
            if(remediation_path && request.checkpoint.remediation_cycle == 0) {
                result.outcome = StageOutcome::NeedsRemediation;
                result.acceptance_decision = "rejected";
                result.finding_ids = {"finding-incomplete-artifact"};
            } else {
                result.acceptance_decision = "accepted";
                result.pins.acceptance_report_digest = "sha256:acceptance-report-v1";
            }
            break;
        case HarnessStage::Remediation:
            result.pins.plan_digest = "sha256:plan-v2";
            result.pins.approval_decision_id = "approval-remediation-v2";
            break;
        case HarnessStage::Reexecution:
            result.pins.artifact_manifest_digest = "sha256:artifact-v2";
            result.effect_receipt_digest = "sha256:effect-reexecution-v2";
            break;
        case HarnessStage::Reverification:
            result.acceptance_decision = "accepted";
            result.pins.acceptance_report_digest = "sha256:acceptance-report-v2";
            break;
        case HarnessStage::Judge:
            result.pins.judge_report_digest = "sha256:judge-report-v1";
            break;
        case HarnessStage::Operations:
            result.pins.operations_snapshot_digest = "sha256:operations-v1";
            break;
        case HarnessStage::Intake:
        case HarnessStage::Complete:
            break;
    }
    return result;
}

inline bool side_effecting(harness::HarnessStage stage) {
    return stage == harness::HarnessStage::Execution ||
           stage == harness::HarnessStage::Reexecution;
}

inline harness::HarnessPortRegistry ports(
    const std::shared_ptr<PortCounters>& counters,
    bool remediation_path = true,
    bool reconcile_side_effects = true,
    bool await_plan_approval_once = false) {
    using namespace harness;
    HarnessPortRegistry registry;
    for(std::size_t index = 0;
        index < static_cast<std::size_t>(HarnessStage::Complete); ++index) {
        const auto stage = static_cast<HarnessStage>(index);
        const bool effect = side_effecting(stage);
        auto execute = [counters, remediation_path,
                        await_plan_approval_once](const HarnessStageRequest& request) {
            ++counters->execute[request.stage];
            if(await_plan_approval_once && request.stage == HarnessStage::PlanApproval &&
               request.attempt == 1) {
                HarnessStageResult waiting;
                waiting.outcome = StageOutcome::AwaitingApproval;
                waiting.output_digest = "sha256:approval-request";
                return waiting;
            }
            return successful(request, remediation_path);
        };
        CallbackHarnessStagePort::Reconcile reconcile;
        if(effect && reconcile_side_effects) {
            reconcile = [counters, remediation_path](const HarnessStageRequest& request)
                -> std::optional<HarnessStageResult> {
                ++counters->reconcile[request.stage];
                return successful(request, remediation_path);
            };
        }
        const auto bound = registry.bind(stage, std::make_shared<CallbackHarnessStagePort>(
            "port-" + harness_stage_name(stage), effect, execute, reconcile));
        if(!bound) throw std::runtime_error("test port binding failed");
    }
    return registry;
}

}  // namespace phase4_harness_test
