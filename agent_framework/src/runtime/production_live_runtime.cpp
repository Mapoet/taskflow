#include "agent/runtime/production_live_runtime.hpp"

#include <utility>

#include "agent/contracts/contract.hpp"
#include "agent/conversation/production_bridge.hpp"

namespace agent_framework::runtime {
namespace {
std::string request_digest(const conversation::HarnessSupportedTurnRequest& request) {
    return contracts::canonical_digest({
        {"schema", "agent.production_turn/v1"},
        {"tenant_id", request.turn.identity.tenant_id},
        {"conversation_id", request.turn.identity.conversation_id},
        {"turn_id", request.turn.turn_id},
        {"task_id", request.turn.task_id},
        {"run_id", request.turn.run_id},
        {"input", request.turn.input},
        {"profile", conversation::name(request.turn.profile)}}).value_or("");
}

conversation::ModelTurnOutcome project(const harness::HarnessRunResult& result) {
    conversation::ModelTurnOutcome outcome;
    outcome.task_completion_verified = false;
    const auto& checkpoint = result.checkpoint;
    const auto receipt = !checkpoint.pins.operations_snapshot_digest.empty()
        ? checkpoint.pins.operations_snapshot_digest
        : checkpoint.pins.artifact_manifest_digest;
    switch(result.state) {
    case harness::HarnessState::Completed:
        outcome.reason = conversation::ModelTurnStopReason::EndTurn;
        if(!receipt.empty()) outcome.tool_receipt_refs.push_back(receipt);
        break;
    case harness::HarnessState::Running:
        outcome.reason = conversation::ModelTurnStopReason::ToolRequested;
        if(!checkpoint.harness_id.empty())
            outcome.tool_receipt_refs.push_back("harness:" + checkpoint.harness_id);
        break;
    case harness::HarnessState::AwaitingApproval:
        outcome.reason = conversation::ModelTurnStopReason::AwaitingApproval;
        if(!checkpoint.harness_id.empty())
            outcome.tool_receipt_refs.push_back("harness:" + checkpoint.harness_id);
        break;
    case harness::HarnessState::AwaitingExternal:
        outcome.reason = conversation::ModelTurnStopReason::AwaitingExternal;
        if(!checkpoint.harness_id.empty())
            outcome.tool_receipt_refs.push_back("harness:" + checkpoint.harness_id);
        break;
    case harness::HarnessState::Cancelled:
        outcome.reason = conversation::ModelTurnStopReason::Cancelled;
        break;
    case harness::HarnessState::ManualReview:
    case harness::HarnessState::Rejected:
    case harness::HarnessState::Failed:
        outcome.reason = conversation::ModelTurnStopReason::GuardStopped;
        break;
    }
    return outcome;
}
}  // namespace

harness::TaskClosureDecision evaluate_production_task_closure(
    harness::ProductionWorkflowInputRepository& repository,
    const contracts::ContractIdentity& identity,
    std::string_view acceptance_contract_digest,
    conversation::TaskExecutionProfile profile,
    const harness::HarnessCheckpoint& checkpoint) {
    harness::TaskClosureDecision unavailable;
    unavailable.state = harness::TaskTerminalState::ManualReview;
    unavailable.reason_code = "durable_closure_evidence_incomplete";
    const auto acceptance = repository.acceptance_contract(
        identity, acceptance_contract_digest);
    const auto report = repository.acceptance_report(
        identity, checkpoint.pins.acceptance_report_digest);
    const auto assurance_checkpoint = repository.assurance_checkpoint(
        identity, checkpoint.pins.acceptance_report_digest);
    std::string error;
    const auto contract = acceptance
        ? conversation::closure_contract_from(*acceptance, profile, &error)
        : std::nullopt;
    if(!contract || !report || !assurance_checkpoint ||
       report->decision != assurance::AcceptanceDecision::Accepted ||
       assurance_checkpoint->state != assurance::AssuranceWorkflowState::Completed)
        return unavailable;

    harness::ClosureFacts facts;
    facts.checkpoint = checkpoint;
    facts.last_progress_revision = checkpoint.revision;
    if(!checkpoint.pins.artifact_manifest_digest.empty())
        facts.artifact_refs = {checkpoint.pins.artifact_manifest_digest};
    facts.finding_refs = checkpoint.unresolved_findings;
    if(assurance_checkpoint->resolution)
        facts.limitations = assurance_checkpoint->resolution->residual_risks;
    for(const auto& evidence : assurance_checkpoint->evidence) {
        if(evidence.outcome != assurance::FindingOutcome::Pass ||
           !evidence.independent || evidence.content_digest.empty() ||
           evidence.source_kind.empty() || evidence.source_locator.empty() ||
           evidence.oracle_strength == assurance::OracleStrength::UncalibratedClaim)
            continue;
        facts.strong_evidence_refs.push_back(evidence.content_digest);
        facts.criterion_verdicts.push_back({
            evidence.criterion_id, "pass", {evidence.content_digest},
            facts.artifact_refs, evidence.source_kind, evidence.source_locator,
            checkpoint.pins.acceptance_report_digest, checkpoint.revision});
    }
    return harness::TaskClosureController().evaluate(*contract, facts);
}

ProductionLiveRuntime::ProductionLiveRuntime(
    ProductionRuntimeResources resources,
    harness::Phase4HarnessRuntime harness_runtime,
    harness::ProductionBuildReport report,
    std::shared_ptr<tool_runtime::IncrementalResultViewAssembler> result_view,
    std::shared_ptr<conversation::TaskControlService> task_control,
    std::shared_ptr<conversation::TaskPlanningService> task_planning,
    std::shared_ptr<recovery::DurableTaskStateCoordinator> task_coordinator)
    : resources_(std::move(resources)),
      harness_runtime_(std::move(harness_runtime)),
      report_(std::move(report)),
      result_view_(std::move(result_view)),
      task_control_service_(std::move(task_control)),
      task_planning_service_(std::move(task_planning)),
      durable_task_coordinator_(std::move(task_coordinator)) {}

ProductionRuntimeBuildResult ProductionLiveRuntime::build(
    ProductionRuntimeResources resources) {
    ProductionRuntimeBuildResult result;
    if(resources.lifetime_anchors.empty()) {
        result.error = "production_runtime_lifetime_anchors_required";
        return result;
    }
    auto* durable_inputs = dynamic_cast<harness::SQLiteProductionWorkflowInputRepository*>(
        resources.dependencies.input_repository);
    if(resources.dependencies.input_repository && !durable_inputs) {
        result.error = "production_mutable_input_repository_required";
        return result;
    }
    harness::DefaultProductionCompositionBuilder builder(resources.dependencies);
    auto runtime = builder.build(resources.boundaries, &result.report, &result.error);
    if(!runtime || !result.report.ready || result.report.deployment_manifest_digest.empty()) {
        if(result.error.empty()) result.error = "production_composition_not_ready";
        return result;
    }
    std::shared_ptr<tool_runtime::IncrementalResultViewAssembler> result_view;
    std::shared_ptr<conversation::TaskControlService> task_control;
    std::shared_ptr<conversation::TaskPlanningService> task_planning;
    const auto& dependencies = resources.dependencies;
    if(dependencies.task_registry && dependencies.invocation_store &&
       dependencies.execution_control_store) {
        if(dependencies.incremental_result_store)
            result_view = std::make_shared<tool_runtime::IncrementalResultViewAssembler>(
                *dependencies.incremental_result_store);
        task_control = std::make_shared<conversation::TaskControlService>(
            *dependencies.task_registry, *dependencies.invocation_store,
            *dependencies.execution_control_store,
            result_view.get());
    }
    if(!durable_inputs) {
        result.error = "production_mutable_input_repository_required";
        return result;
    }
    task_planning = std::make_shared<conversation::TaskPlanningService>(
        *dependencies.task_registry, *dependencies.cognition_workflow, *durable_inputs);
    if(!resources.task_coordination_journal) {
        result.error = "production_task_coordination_journal_required";
        return result;
    }
    auto task_coordinator = std::make_shared<recovery::DurableTaskStateCoordinator>(
        *dependencies.task_registry, *resources.task_coordination_journal);
    auto owned = std::shared_ptr<ProductionLiveRuntime>(new ProductionLiveRuntime(
        std::move(resources), std::move(*runtime), result.report,
        std::move(result_view), std::move(task_control), std::move(task_planning),
        std::move(task_coordinator)));
    result.runtime = std::move(owned);
    return result;
}

conversation::HarnessSupportedTurnRuntime::Executor
ProductionLiveRuntime::response_executor() {
    const auto self = shared_from_this();
    return [self](const auto& request) { return self->execute(request, false); };
}

conversation::HarnessSupportedTurnRuntime::Executor
ProductionLiveRuntime::long_task_executor() {
    const auto self = shared_from_this();
    return [self](const auto& request) { return self->execute(request, true); };
}

conversation::ModelTurnOutcome ProductionLiveRuntime::execute(
    const conversation::HarnessSupportedTurnRequest& request,
    bool task_scoped) {
    if(request.turn.identity.tenant_id.empty() || request.turn.turn_id.empty() ||
       (task_scoped && request.turn.task_id.empty())) {
        conversation::ModelTurnOutcome invalid;
        invalid.reason = conversation::ModelTurnStopReason::GuardStopped;
        return invalid;
    }
    conversation::TaskPlanningResult plan_result;
    if(task_scoped) {
        const auto task = resources_.dependencies.task_registry->load(
            request.turn.identity, request.turn.task_id);
        if(!task) {
            conversation::ModelTurnOutcome invalid;
            invalid.reason = conversation::ModelTurnStopReason::GuardStopped;
            return invalid;
        }
        plan_result = task_planning_service_->plan(
            *task, resources_.planning_policy);
        if(plan_result.state == conversation::TaskPlanningState::AwaitingClarification) {
            conversation::ModelTurnOutcome waiting;
            waiting.reason = conversation::ModelTurnStopReason::AwaitingInput;
            if(!plan_result.clarification_questions.empty()) {
                waiting.clarification = plan_result.clarification_questions.front();
                waiting.candidate_answer = plan_result.clarification_questions.front();
            }
            return waiting;
        }
        if(plan_result.state == conversation::TaskPlanningState::AwaitingApproval) {
            conversation::ModelTurnOutcome waiting;
            waiting.reason = conversation::ModelTurnStopReason::AwaitingApproval;
            return waiting;
        }
        if(plan_result.state != conversation::TaskPlanningState::Planned) {
            conversation::ModelTurnOutcome failed;
            failed.reason = conversation::ModelTurnStopReason::GuardStopped;
            return failed;
        }
    }
    const auto digest = request_digest(request);
    if(digest.empty()) {
        conversation::ModelTurnOutcome invalid;
        invalid.reason = conversation::ModelTurnStopReason::GuardStopped;
        return invalid;
    }
    const auto harness_id = task_scoped
        ? "task:" + request.turn.task_id
        : "turn:" + request.turn.turn_id;
    harness::HarnessStart start;
    start.metadata.identity.tenant_id = request.turn.identity.tenant_id;
    start.metadata.identity.principal_id = request.turn.identity.conversation_id;
    start.metadata.identity.task_id = request.turn.task_id.empty()
        ? request.turn.turn_id : request.turn.task_id;
    start.metadata.identity.run_id = request.turn.run_id;
    start.metadata.identity.plan_id = request.turn.task_id + ":plan";
    start.metadata.extensions = {{"conversation_id", request.turn.identity.conversation_id},
                                 {"turn_id", request.turn.turn_id}};
    start.harness_id = harness_id;
    start.intake_digest = digest;
    start.acceptance_contract_digest = digest;
    if(task_scoped) {
        const auto intake = resources_.dependencies.input_repository->intake(
            start.metadata.identity);
        if(!intake || plan_result.plan_digest.empty() ||
           plan_result.acceptance_contract_digest.empty() ||
           plan_result.context_projection_digest.empty()) {
            conversation::ModelTurnOutcome invalid;
            invalid.reason = conversation::ModelTurnStopReason::GuardStopped;
            invalid.candidate_answer = "production_preplanned_context_incomplete";
            return invalid;
        }
        start.intake_digest = planning::encode(*intake)
            .value("canonical_digest", "");
        start.acceptance_contract_digest = plan_result.acceptance_contract_digest;
        start.initial_stage = harness::HarnessStage::PlanApproval;
        start.plan_digest = plan_result.plan_digest;
        start.completed_stage_records = {
            {harness::HarnessStage::Intake, 1, harness::StageOutcome::Succeeded,
             {}, {}, start.intake_digest},
            {harness::HarnessStage::Cognition, 1, harness::StageOutcome::Succeeded,
             {}, {}, plan_result.plan_digest}};
    }
    start.profile_revision_digest = report_.deployment_manifest_digest;
    start.prompt_revision_digest = report_.composition_manifest_digest;
    std::lock_guard<std::mutex> lock(execute_mutex_);
    const auto result = harness_runtime_.run(start);
    auto outcome = project(result);
    if(task_scoped) {
        const auto current_task = resources_.dependencies.task_registry->load(
            request.turn.identity, request.turn.task_id);
        const auto run_record = resources_.dependencies.run_store->load(request.turn.run_id);
        if(!current_task || !run_record) {
            outcome.reason = conversation::ModelTurnStopReason::GuardStopped;
            return outcome;
        }
        recovery::CorrelatedStateEvent event;
        event.identity = request.turn.identity;
        event.task_id = request.turn.task_id;
        event.turn_id = request.turn.turn_id;
        event.run_id = request.turn.run_id;
        event.harness_id = result.checkpoint.harness_id;
        event.task_revision = current_task->revision;
        event.run_state = run_record->checkpoint.state;
        event.harness_state = result.checkpoint.state;
        if(result.checkpoint.state == harness::HarnessState::Completed) {
            const auto closure = evaluate_production_task_closure(
                *resources_.dependencies.input_repository, start.metadata.identity,
                plan_result.acceptance_contract_digest, request.turn.profile,
                result.checkpoint);
            event.closure_verified = closure.state ==
                harness::TaskTerminalState::CompletedVerified;
        }
        event.source_event_id = result.checkpoint.harness_id + ":" +
            std::to_string(result.checkpoint.revision);
        event.turn_phase = outcome.reason == conversation::ModelTurnStopReason::EndTurn
            ? conversation::TurnPhase::Completed
            : (outcome.reason == conversation::ModelTurnStopReason::AwaitingInput ||
               outcome.reason == conversation::ModelTurnStopReason::AwaitingApproval)
                ? conversation::TurnPhase::AwaitingInput
                : outcome.reason == conversation::ModelTurnStopReason::AwaitingExternal
                    ? conversation::TurnPhase::AwaitingTool
                    : conversation::TurnPhase::Running;
        for(const auto& effect : result.checkpoint.outbox) {
            event.pending_effect = event.pending_effect ||
                effect.state == harness::OutboxState::Pending;
            event.unknown_effect = event.unknown_effect ||
                effect.state == harness::OutboxState::Unknown;
        }
        const auto invocations = resources_.dependencies.invocation_store->query(
            {request.turn.identity.tenant_id, request.turn.identity.conversation_id,
             request.turn.run_id, {}, 1000});
        std::vector<tool_runtime::PartialResultRef> partials;
        for(const auto& invocation : invocations) {
            event.invocation_active = event.invocation_active ||
                (invocation.state != tool_runtime::InvocationState::Verified &&
                 invocation.state != tool_runtime::InvocationState::Failed &&
                 invocation.state != tool_runtime::InvocationState::Cancelled &&
                 invocation.state != tool_runtime::InvocationState::ManualReview);
            const auto invocation_partials =
                resources_.dependencies.invocation_store->partial_results(
                    invocation.invocation_id);
            partials.insert(partials.end(), invocation_partials.begin(),
                            invocation_partials.end());
        }
        if(outcome.reason == conversation::ModelTurnStopReason::EndTurn &&
           outcome.candidate_answer.empty() && result_view_ && !partials.empty()) {
            const auto view = result_view_->assemble(
                request.turn.identity.tenant_id, partials);
            if(auto streams = view.find("streams"); streams != view.end() &&
               streams->is_array()) {
                for(const auto& stream : *streams) {
                    const auto preview = stream.value("preview", "");
                    if(preview.empty()) continue;
                    if(!outcome.candidate_answer.empty())
                        outcome.candidate_answer += "\n";
                    outcome.candidate_answer += preview;
                }
            }
        }
        if(outcome.reason == conversation::ModelTurnStopReason::EndTurn &&
           outcome.candidate_answer.empty()) {
            outcome.reason = conversation::ModelTurnStopReason::GuardStopped;
            outcome.candidate_answer = "execution_completed_without_deliverable";
        }
        const auto decision = task_state_coordinator_.observe(event);
        std::string coordination_error;
        if(!durable_task_coordinator_->publish(event, decision, &coordination_error))
            outcome.reason = conversation::ModelTurnStopReason::GuardStopped;
    }
    return outcome;
}

}  // namespace agent_framework::runtime
