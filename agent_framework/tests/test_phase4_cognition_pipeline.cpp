#include <algorithm>
#include <cassert>
#include <filesystem>
#include <memory>

#include "phase4_cognition_pipeline_test_support.hpp"
#include "agent/approval/store.hpp"
#include "agent/internal/platform_io.hpp"

namespace {
using namespace phase4_cognition_test;

struct Fixture {
    memory_v2::MemoryProviderRegistry providers;
    memory_v2::MemoryViewEngine views{providers};
    InvestigatorRegistry investigators;
    std::shared_ptr<RepoInvestigator> repo = std::make_shared<RepoInvestigator>();
    std::shared_ptr<DocsInvestigator> docs = std::make_shared<DocsInvestigator>();
    InMemoryEvidenceStore evidence;
    InMemoryPlanStore plans;
    InMemoryCognitionCheckpointStore checkpoints;
    ScriptedStageModel model;

    Fixture() {
        assert(investigators.register_investigator(repo));
        assert(investigators.register_investigator(docs));
    }
};
}

int main() {
    using namespace phase4_cognition_test;
    {
        Fixture fixture;
        script_success(fixture.model, true);
        MultiStageCognitionWorkflow workflow(
            fixture.views, fixture.investigators, fixture.evidence, fixture.plans,
            fixture.checkpoints, fixture.model);
        std::vector<CognitionPipelineEvent> events;
        CognitionPipelineOptions options;
        options.pipeline_id = "pipeline-success";
        options.max_critic_revisions = 2;
        options.event_sink = [&](const auto& event) { events.push_back(event); };
        const auto task = intake();
        const auto result = workflow.run(task, subject(task), options);
        assert(result.state == CognitionPipelineState::Approved);
        assert(result.plan && result.plan->plan_revision == 2);
        assert(result.understanding);
        assert(result.checkpoint.tool_calls_used == 2);
        assert(result.checkpoint.evidence_ids.size() == 2);
        assert(fixture.plans.history(task.metadata.identity).size() == 2);
        assert(fixture.repo->rounds == std::vector<std::uint64_t>{1});
        assert(fixture.docs->rounds == std::vector<std::uint64_t>{2});
        const auto doc = fixture.evidence.get(task.metadata, "ev-doc");
        assert(doc && !doc->instruction_authority);
        assert(std::find(result.plan->nodes[1].input_contracts.begin(),
                         result.plan->nodes[1].input_contracts.end(),
                         "basis:ev-doc") != result.plan->nodes[1].input_contracts.end());
        const auto critic_request = std::find_if(
            fixture.model.requests.begin(), fixture.model.requests.end(), [](const auto& request) {
                return request.stage == CognitionStage::Critique && request.iteration == 0;
            });
        assert(critic_request != fixture.model.requests.end());
        assert(critic_request->independence.forbidden_groups ==
               std::vector<std::string>{"planner"});
        assert(critic_request->independence.forbidden_providers ==
               std::vector<std::string>{"planner-provider"});
        assert(std::any_of(events.begin(), events.end(), [](const auto& event) {
            return event.event_type == "plan_approved";
        }));

        // Terminal replay is idempotent: no LLM or investigator is called again.
        const auto calls = fixture.model.requests.size();
        const auto replay = workflow.run(task, subject(task), options);
        assert(replay.state == CognitionPipelineState::Approved);
        assert(fixture.model.requests.size() == calls);

        auto changed = task;
        changed.user_goal = "silently replace the goal";
        const auto mismatch = workflow.run(changed, subject(changed), options);
        assert(mismatch.error_code == "checkpoint_input_mismatch");
    }

    {
        // A claim that cites evidence outside the deterministic bundle fails closed.
        Fixture fixture;
        fixture.model.push(CognitionStage::Intake, intake_output());
        fixture.model.push(CognitionStage::Strategy, strategy_output());
        fixture.model.push(CognitionStage::Synthesis, synthesis_output(true));
        MultiStageCognitionWorkflow workflow(
            fixture.views, fixture.investigators, fixture.evidence, fixture.plans,
            fixture.checkpoints, fixture.model);
        CognitionPipelineOptions options;
        options.pipeline_id = "pipeline-hallucinated-evidence";
        const auto task = intake("task-hallucination");
        const auto result = workflow.run(task, subject(task), options);
        assert(result.state == CognitionPipelineState::Failed);
        assert(result.error_code == "synthesis_output_invalid");
        assert(!result.plan);
    }

    {
        // A structurally present but wrongly typed field is not allowed to escape as an
        // exception or truthy coercion.
        Fixture fixture;
        auto malformed = intake_output();
        malformed["clarification_required"] = "false";
        fixture.model.push(CognitionStage::Intake, std::move(malformed));
        MultiStageCognitionWorkflow workflow(
            fixture.views, fixture.investigators, fixture.evidence, fixture.plans,
            fixture.checkpoints, fixture.model);
        CognitionPipelineOptions options;
        options.pipeline_id = "pipeline-malformed-intake";
        const auto task = intake("task-malformed-intake");
        const auto result = workflow.run(task, subject(task), options);
        assert(result.state == CognitionPipelineState::Failed);
        assert(result.error_code == "intake_output_invalid");
    }

    {
        // Strategy cannot expand the user's authority merely by naming a registered tool.
        Fixture fixture;
        fixture.model.push(CognitionStage::Intake, intake_output());
        fixture.model.push(CognitionStage::Strategy, strategy_output());
        MultiStageCognitionWorkflow workflow(
            fixture.views, fixture.investigators, fixture.evidence, fixture.plans,
            fixture.checkpoints, fixture.model);
        CognitionPipelineOptions options;
        options.pipeline_id = "pipeline-capability-denied";
        auto task = intake("task-capability-denied");
        task.granted_authorities.erase(std::remove(
            task.granted_authorities.begin(), task.granted_authorities.end(), "external_read"),
            task.granted_authorities.end());
        const auto result = workflow.run(task, subject(task), options);
        assert(result.state == CognitionPipelineState::Failed);
        assert(result.error_code == "investigator_capability_denied");
    }

    {
        // The independent critic is advisory but still cannot manufacture evidence refs.
        Fixture fixture;
        fixture.model.push(CognitionStage::Intake, intake_output());
        fixture.model.push(CognitionStage::Strategy, strategy_output());
        fixture.model.push(CognitionStage::Synthesis, synthesis_output());
        fixture.model.push(CognitionStage::Boundary, boundary_output());
        fixture.model.push(CognitionStage::Planning, plan_output());
        auto critic = critique_output(false);
        critic["findings"][0]["evidence_ids"] = {"ev-invented"};
        fixture.model.push(CognitionStage::Critique, std::move(critic),
                           "critic-provider", "critic-model", "critic");
        MultiStageCognitionWorkflow workflow(
            fixture.views, fixture.investigators, fixture.evidence, fixture.plans,
            fixture.checkpoints, fixture.model);
        CognitionPipelineOptions options;
        options.pipeline_id = "pipeline-critic-invented-evidence";
        const auto task = intake("task-critic-invented-evidence");
        const auto result = workflow.run(task, subject(task), options);
        assert(result.state == CognitionPipelineState::Failed);
        assert(result.error_code == "critique_output_invalid");
    }

    {
        // Clarification is a durable stop. Supplying answers creates a new Intake invocation.
        Fixture fixture;
        fixture.model.push(CognitionStage::Intake, intake_output(true));
        fixture.model.push(CognitionStage::Intake, intake_output(false));
        fixture.model.push(CognitionStage::Strategy, strategy_output());
        fixture.model.push(CognitionStage::Synthesis, synthesis_output());
        fixture.model.push(CognitionStage::Boundary, boundary_output());
        fixture.model.push(CognitionStage::Planning, plan_output());
        fixture.model.push(CognitionStage::Critique, critique_output(true),
                           "critic-provider", "critic-model", "critic");
        MultiStageCognitionWorkflow workflow(
            fixture.views, fixture.investigators, fixture.evidence, fixture.plans,
            fixture.checkpoints, fixture.model);
        CognitionPipelineOptions options;
        options.pipeline_id = "pipeline-clarification";
        const auto task = intake("task-clarification");
        const auto waiting = workflow.run(task, subject(task), options);
        assert(waiting.state == CognitionPipelineState::AwaitingClarification);
        assert(waiting.clarification_questions.size() == 1);
        options.clarification_answers = {{"compatibility_window", "one release"}};
        const auto resumed = workflow.run(task, subject(task), options);
        assert(resumed.state == CognitionPipelineState::Approved);
        const auto intake_calls = std::count_if(
            fixture.model.requests.begin(), fixture.model.requests.end(), [](const auto& request) {
                return request.stage == CognitionStage::Intake;
            });
        assert(intake_calls == 2);
    }

    {
        // High-risk plans stop durably. A user modification creates a new immutable plan
        // revision and is independently criticized again before approval.
        Fixture fixture;
        script_success(fixture.model, false, true);
        fixture.model.push(CognitionStage::Revision, plan_output(true, false));
        fixture.model.push(CognitionStage::Critique, critique_output(true),
                           "critic-provider", "critic-model", "critic");
        MultiStageCognitionWorkflow workflow(
            fixture.views, fixture.investigators, fixture.evidence, fixture.plans,
            fixture.checkpoints, fixture.model);
        CognitionPipelineOptions options;
        options.pipeline_id = "pipeline-hitl-revision";
        const auto task = intake("task-hitl-revision");
        const auto waiting = workflow.run(task, subject(task), options);
        assert(waiting.state == CognitionPipelineState::AwaitingApproval);
        assert(waiting.plan && waiting.plan->plan_revision == 1);
        options.plan_revision_request = {{"request", "lower risk by isolating the shim"}};
        const auto revised = workflow.run(task, subject(task), options);
        assert(revised.state == CognitionPipelineState::Approved);
        assert(revised.plan && revised.plan->plan_revision == 2);
        assert(fixture.plans.history(task.metadata.identity).size() == 2);
    }

    {
        // Approval is accepted only through a caller-supplied deterministic validator.
        Fixture fixture;
        script_success(fixture.model, false, true);
        MultiStageCognitionWorkflow workflow(
            fixture.views, fixture.investigators, fixture.evidence, fixture.plans,
            fixture.checkpoints, fixture.model);
        CognitionPipelineOptions options;
        options.pipeline_id = "pipeline-hitl-approval";
        const auto task = intake("task-hitl-approval");
        assert(workflow.run(task, subject(task), options).state ==
               CognitionPipelineState::AwaitingApproval);
        options.approval_decision_id = "approval-42";
        options.approval_validator = [](std::string_view digest, std::string_view decision) {
            return !digest.empty() && decision == "approval-42";
        };
        const auto approved = workflow.run(task, subject(task), options);
        assert(approved.state == CognitionPipelineState::Approved);
        assert(approved.checkpoint.approval_decision_id == "approval-42");
    }
    {
        // Production resolution is store-backed and binds identity, request digest,
        // policy revision, plan digest, decision state and expiry.
        Fixture fixture;
        script_success(fixture.model, false, true);
        MultiStageCognitionWorkflow workflow(
            fixture.views, fixture.investigators, fixture.evidence, fixture.plans,
            fixture.checkpoints, fixture.model);
        CognitionPipelineOptions options;
        options.pipeline_id = "pipeline-store-approval";
        const auto task = intake("task-store-approval");
        const auto waiting = workflow.run(task, subject(task), options);
        assert(waiting.state == CognitionPipelineState::AwaitingApproval && waiting.plan);
        const auto root = std::filesystem::temp_directory_path() /
            ("agent-phase4-cognition-approval-" +
             std::to_string(internal::current_process_id()));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root);
        approval::SQLiteApprovalStore approvals((root / "approval.sqlite3").string());
        approval::ApprovalRequest request;
        request.metadata = task.metadata;
        request.approval_id = "approval-store-1";
        request.request_kind = "plan";
        request.requester_id = "requester-a";
        request.scope = "plan:" + task.metadata.identity.plan_id;
        request.reason = "high-risk plan";
        request.risk_level = "high";
        request.policy_revision = "policy-v1";
        request.plan_digest = planning::encode(*waiting.plan).at("canonical_digest");
        request.created_at = "2026-08-12T00:00:00Z";
        request.expires_at = "2026-08-13T00:00:00Z";
        assert(approvals.put_request(request));
        approval::ApprovalDecision decision;
        decision.metadata = request.metadata;
        decision.approval_id = request.approval_id;
        decision.request_digest = approval::encode(request).at("canonical_digest");
        decision.reviewer_id = "reviewer-a";
        decision.decision = approval::Decision::Approved;
        decision.scope = request.scope;
        decision.reason = "reviewed";
        decision.policy_revision = request.policy_revision;
        decision.plan_digest = request.plan_digest;
        decision.decided_at = "2026-08-12T01:00:00Z";
        decision.expires_at = request.expires_at;
        assert(approvals.decide(decision, 0));
        options.approval_decision_id = request.approval_id;
        StoreBackedPlanApprovalResolver resolver(approvals);
        options.approval_resolver = &resolver;
        options.now = [] { return std::string("2026-08-12T02:00:00Z"); };
        assert(workflow.run(task, subject(task), options).state ==
               CognitionPipelineState::Approved);
        std::string approval_error;
        assert(!options.approval_resolver->approved(
            intake("other-task").metadata.identity, request.plan_digest,
            request.approval_id, "2026-08-12T02:00:00Z", &approval_error));
        assert(!options.approval_resolver->approved(
            task.metadata.identity, request.plan_digest, request.approval_id,
            "2026-08-14T00:00:00Z", &approval_error));
        std::filesystem::remove_all(root, ec);
    }
    return 0;
}
