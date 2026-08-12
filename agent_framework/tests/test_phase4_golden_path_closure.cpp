#include <agent/harness/task_closure.hpp>
#include <agent/a2a/orchestration.hpp>
#include <agent/agent/child_task.hpp>
#include "phase4_harness_test_support.hpp"

#include <cassert>

using namespace agent_framework::harness;

namespace {
TaskClosureContract contract(std::string id) {
    TaskClosureContract c;
    c.metadata=phase4_harness_test::metadata(id);
    c.contract_id="closure-"+id; c.revision="r1"; c.task_class="artifact_delivery";
    c.deliverables={"deliverable"}; c.mandatory_criteria={"exists","valid"};
    c.verification_methods={{"exists",{"artifact_oracle"}},{"valid",{"content_oracle"}}};
    c.clarification_policy="ask"; return c;
}
ClosureFacts complete(const TaskClosureContract& c) {
    ClosureFacts f; f.checkpoint.metadata=c.metadata; f.checkpoint.harness_id="golden";
    f.checkpoint.revision=9; f.checkpoint.state=HarnessState::Completed;
    f.checkpoint.next_stage=HarnessStage::Complete; f.checkpoint.judge_required=true;
    f.checkpoint.pins.intake_digest="i"; f.checkpoint.pins.plan_digest="p";
    f.checkpoint.pins.acceptance_contract_digest="c"; f.checkpoint.pins.memory_snapshot_id="m";
    f.checkpoint.pins.memory_view_digest="mv"; f.checkpoint.pins.approval_decision_id="a";
    f.checkpoint.pins.artifact_manifest_digest="am"; f.checkpoint.pins.acceptance_report_digest="ar";
    f.checkpoint.pins.judge_report_digest="j"; f.checkpoint.pins.operations_snapshot_digest="o";
    for(auto stage:{HarnessStage::Intake,HarnessStage::Cognition,HarnessStage::PlanApproval,
        HarnessStage::Execution,HarnessStage::MemoryUpdate,HarnessStage::Assurance,
        HarnessStage::Judge,HarnessStage::Operations})
        f.checkpoint.stage_records.push_back({stage,1,StageOutcome::Succeeded});
    f.satisfied_criteria=c.mandatory_criteria; f.strong_evidence_refs={"sha256:evidence"};
    f.artifact_refs={"sha256:artifact"}; f.last_progress_revision=9; return f;
}
}

int main() {
    TaskClosureController controller;
    // Golden A: an artifact can complete only after every deterministic gate closes.
    auto a=contract("golden-file"); auto af=complete(a);
    assert(controller.evaluate(a,af).state==TaskTerminalState::CompletedVerified);
    af.artifact_refs.clear();
    assert(controller.evaluate(a,af).state==TaskTerminalState::ManualReview);

    // Golden B: failed verification selects the smallest bounded remediation, then closes.
    auto b=contract("golden-code"); auto bf=complete(b);
    bf.checkpoint.state=HarnessState::Running; bf.verification_failed=true;
    bf.satisfied_criteria={"exists"}; bf.finding_refs={"compile_failure"};
    assert(controller.evaluate(b,bf).state==TaskTerminalState::MinimalRemediation);
    bf.checkpoint.remediation_cycle=b.max_remediation_cycles;
    assert(controller.evaluate(b,bf).state==TaskTerminalState::FailedVerification);
    bf=complete(b);
    assert(controller.evaluate(b,bf).state==TaskTerminalState::CompletedVerified);

    // Golden C: an unavailable external dependency is resumable, never reported as success.
    auto c=contract("golden-external"); auto cf=complete(c);
    cf.checkpoint.state=HarnessState::Running; cf.external_blockers={"provider_credentials"};
    const auto blocked=controller.evaluate(c,cf);
    assert(blocked.state==TaskTerminalState::BlockedExternal);
    assert(blocked.recommended_next_action.find("resume")!=std::string::npos);
    cf.external_blockers.clear();
    cf.missing_facts={"credential reference"};
    assert(controller.evaluate(c,cf).state==TaskTerminalState::NeedsUserInput);
    std::string reason;
    assert(!agent_framework::a2a::accept_remote_completion({true,true,false,false,""},&reason));
    assert(reason=="local_artifact_verification_required");
    assert(agent_framework::a2a::accept_remote_completion({true,true,true,true,"sha256:local"}));
    agent_framework::ChildTaskResult child;
    child.status=agent_framework::ChildTaskStatus::Completed;
    child.outputs={{"task_completion_verified",false},{"completion_authority","none"}};
    assert(child.ok() && !child.verified_complete());
    child.outputs={{"task_completion_verified",true},{"completion_authority","task_closure_controller"}};
    assert(child.verified_complete());
    return 0;
}
