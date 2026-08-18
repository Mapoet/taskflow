#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <filesystem>
#include <memory>

#include "agent/runtime/production_live_runtime.hpp"
#include "agent/internal/platform_io.hpp"
#include "phase4_harness_test_support.hpp"
#include "phase4_cognition_pipeline_test_support.hpp"

namespace agent_framework::runtime {
struct ProductionLiveRuntimeTestAccess {
    static ProductionRuntimeBuildResult assemble(
        ProductionRuntimeResources resources,
        harness::Phase4HarnessRuntime runtime,
        harness::ProductionBuildReport report,
        ProductionOwnershipReport ownership) {
        return ProductionLiveRuntime::assemble_validated(
            std::move(resources), std::move(runtime), std::move(report),
            std::move(ownership));
    }
};
}

int main() {
    using namespace agent_framework::runtime;
    ProductionRuntimeResources empty;
    auto no_anchors = ProductionLiveRuntime::build(empty);
    assert(!no_anchors);
    assert(no_anchors.error == "production_runtime_lifetime_anchors_required");

    empty.lifetime_anchors.push_back(std::make_shared<int>(1));
    auto wrong_profile = ProductionLiveRuntime::build(empty);
    assert(!wrong_profile);
    assert(wrong_profile.error == "production_runtime_profile_required");
    empty.deployment_profile = "production";
    auto missing_owner = ProductionLiveRuntime::build(empty);
    assert(!missing_owner);
    assert(missing_owner.error ==
           "production_runtime_owned_resource_missing:conversation_store");
    for(const auto* name : {"conversation_store", "task_registry", "run_store",
             "harness_store", "plan_store", "invocation_store",
             "incremental_result_store", "effect_journal", "approval_store",
             "memory_store", "assurance_store", "judge_store", "telemetry"})
        empty.owned_resources[name] = std::make_shared<int>(1);
    const auto ownership = validate_production_runtime_ownership(empty);
    assert(ownership.ready && ownership.missing.empty());
    assert(!ownership.manifest_digest.empty());
    auto registry = std::make_shared<agent_framework::conversation::SQLiteTaskRegistry>(
        ":memory:");
    empty.dependencies.task_registry = registry.get();
    const auto mismatched = validate_production_runtime_ownership(empty);
    assert(!mismatched.ready && mismatched.mismatched.size() == 1);
    assert(mismatched.mismatched.front() == "task_registry");
    empty.owned_resources["task_registry"] = registry;
    empty.lifetime_anchors.push_back(registry);
    assert(validate_production_runtime_ownership(empty).ready);
    auto missing = ProductionLiveRuntime::build(std::move(empty));
    assert(!missing);
    assert(missing.error.find("production_dependency_missing") != std::string::npos);

    using namespace agent_framework;
    const auto root = std::filesystem::temp_directory_path() /
        ("production-closure-" + std::to_string(internal::current_process_id()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    harness::SQLiteProductionWorkflowInputRepository inputs(
        (root / "inputs.sqlite3").string());
    harness::InMemoryHarnessStore harness_store;
    auto counters = std::make_shared<phase4_harness_test::PortCounters>();
    harness::Phase4HarnessRuntime harness_runtime(
        harness_store, phase4_harness_test::ports(counters, false));
    auto completed = harness_runtime.run(
        phase4_harness_test::start("closure-harness"));
    assert(completed.state == harness::HarnessState::Completed);

    assurance::AcceptanceContract acceptance;
    acceptance.metadata = completed.checkpoint.metadata;
    acceptance.revision = 1;
    acceptance.plan_digest = completed.checkpoint.pins.plan_digest;
    acceptance.criteria = {{"criterion-a", assurance::VerificationLayer::System,
        "verified in the real system", "repository", {"repository"}, "pass", true}};
    const auto acceptance_document = assurance::encode(acceptance);
    const auto acceptance_digest =
        acceptance_document.at("canonical_digest").get<std::string>();
    assert(inputs.put_acceptance_contract(acceptance));

    assurance::AcceptanceReport report;
    report.metadata = completed.checkpoint.metadata;
    report.plan_digest = completed.checkpoint.pins.plan_digest;
    report.acceptance_contract_digest = acceptance_digest;
    report.artifact_manifest_digest =
        completed.checkpoint.pins.artifact_manifest_digest;
    report.memory_snapshot_id = completed.checkpoint.pins.memory_snapshot_id;
    report.verification_view_digest = completed.checkpoint.pins.memory_view_digest;
    report.decision = assurance::AcceptanceDecision::Accepted;
    const auto report_digest = contracts::canonical_digest(
        assurance::encode(report)).value_or("");
    assert(!report_digest.empty());
    assert(inputs.put_acceptance_report(report, report_digest));

    assurance::AssuranceCheckpoint assurance_checkpoint;
    assurance_checkpoint.metadata = completed.checkpoint.metadata;
    assurance_checkpoint.workflow_id = "closure-assurance";
    assurance_checkpoint.revision = completed.checkpoint.revision;
    assurance_checkpoint.state = assurance::AssuranceWorkflowState::Completed;
    assurance_checkpoint.next_stage = assurance::AssuranceStage::Complete;
    assurance_checkpoint.acceptance_contract_digest = acceptance_digest;
    assurance_checkpoint.task_context_digest = "sha256:task-context";
    assurance_checkpoint.artifact_manifest_digest =
        completed.checkpoint.pins.artifact_manifest_digest;
    assurance_checkpoint.memory_snapshot_id =
        completed.checkpoint.pins.memory_snapshot_id;
    assurance_checkpoint.memory_view_digest =
        completed.checkpoint.pins.memory_view_digest;
    assurance_checkpoint.acceptance_report_digest = report_digest;
    assurance_checkpoint.updated_at = "2026-08-16T00:00:00Z";
    assurance_checkpoint.evidence = {{"evidence-a", "criterion-a", "repository",
        "file://workspace/result", "sha256:evidence-a", "2026-08-16T00:00:00Z",
        "2026-08-17T00:00:00Z", assurance::OracleStrength::RealSystem,
        assurance::FindingOutcome::Pass, true}};
    assert(inputs.put_assurance_checkpoint(assurance_checkpoint, report_digest));
    completed.checkpoint.pins.acceptance_report_digest = report_digest;

    const auto unavailable = evaluate_production_task_closure(
        inputs, completed.checkpoint.metadata.identity, "sha256:missing",
        conversation::TaskExecutionProfile::ArtifactDelivery,
        completed.checkpoint);
    assert(unavailable.state == harness::TaskTerminalState::ManualReview);
    const auto verified = evaluate_production_task_closure(
        inputs, completed.checkpoint.metadata.identity, acceptance_digest,
        conversation::TaskExecutionProfile::ArtifactDelivery,
        completed.checkpoint);
    assert(verified.state == harness::TaskTerminalState::CompletedVerified);

    // Positive runtime assembly: the same private assembly stage used by the
    // production builder must create all services and retain every anchor for
    // as long as any exported executor remains alive.
    memory_v2::MemoryProviderRegistry providers;
    memory_v2::MemoryViewEngine views(providers);
    planning::InvestigatorRegistry investigators;
    assert(investigators.register_investigator(
        std::make_shared<phase4_cognition_test::RepoInvestigator>()));
    assert(investigators.register_investigator(
        std::make_shared<phase4_cognition_test::DocsInvestigator>()));
    planning::InMemoryEvidenceStore evidence;
    planning::InMemoryPlanStore plans;
    planning::InMemoryCognitionCheckpointStore cognition_checkpoints;
    phase4_cognition_test::ScriptedStageModel cognition_model;
    phase4_cognition_test::script_success(cognition_model, false);
    auto cognition = std::make_shared<planning::MultiStageCognitionWorkflow>(
        views, investigators, evidence, plans, cognition_checkpoints,
        cognition_model);
    auto tasks = std::make_shared<conversation::SQLiteTaskRegistry>(
        (root / "assembly-tasks.sqlite3").string());
    auto invocations = std::make_shared<tool_runtime::SQLiteInvocationStore>(
        (root / "assembly-invocations.sqlite3").string());
    auto controls = std::make_shared<tool_runtime::SQLiteExecutionControlStore>(
        (root / "assembly-controls.sqlite3").string());
    auto durable_runs = std::make_shared<run::SQLiteRunStore>(
        (root / "assembly-runs.sqlite3").string());
    auto assembly_inputs =
        std::make_shared<harness::SQLiteProductionWorkflowInputRepository>(
            (root / "assembly-inputs.sqlite3").string());
    auto journal = std::make_shared<recovery::SQLiteTaskCoordinationJournal>(
        (root / "assembly-coordination.sqlite3").string());
    auto assembly_harness_store = std::make_shared<harness::InMemoryHarnessStore>();
    auto assembly_counters = std::make_shared<phase4_harness_test::PortCounters>();
    harness::Phase4HarnessRuntime assembly_harness(
        *assembly_harness_store,
        phase4_harness_test::ports(assembly_counters, false));
    auto lifetime = std::make_shared<int>(42);
    std::weak_ptr<int> lifetime_probe = lifetime;
    ProductionRuntimeResources assembly_resources;
    assembly_resources.deployment_profile = "production";
    assembly_resources.dependencies.task_registry = tasks.get();
    assembly_resources.dependencies.run_store = durable_runs.get();
    assembly_resources.dependencies.invocation_store = invocations.get();
    assembly_resources.dependencies.execution_control_store = controls.get();
    assembly_resources.dependencies.input_repository = assembly_inputs.get();
    assembly_resources.dependencies.cognition_workflow = cognition.get();
    assembly_resources.task_coordination_journal = journal.get();
    assembly_resources.planning_policy.requested_deliverables = {"code", "tests"};
    assembly_resources.planning_policy.granted_authorities = {
        "workspace_read", "workspace_write", "repo_read", "repo_write", "external_read"};
    assembly_resources.planning_policy.success_signals = {"all tests pass"};
    assembly_resources.planning_policy.executor_id = "workspace-agent";
    assembly_resources.planning_policy.executor_revision = "r1";
    assurance::AcceptanceContract production_acceptance;
    production_acceptance.revision = 1;
    production_acceptance.criteria = {{"deliverable-present",
        assurance::VerificationLayer::System,"a deliverable is committed",
        "artifact",{"artifact"},"pass",true}};
    assembly_resources.planning_policy.acceptance_contract = production_acceptance;
    assembly_resources.lifetime_anchors = {
        tasks, invocations, controls, assembly_inputs, journal, cognition,
        durable_runs, assembly_harness_store, assembly_counters, lifetime};

    conversation::PersistentTask production_task;
    production_task.identity = {"tenant", "production-conversation"};
    production_task.task_id = "production-task";
    production_task.root_turn_id = "production-turn";
    production_task.current_turn_id = "production-turn";
    production_task.current_run_id = "production-run";
    conversation::TaskRequirementRevision production_requirement;
    production_requirement.identity = production_task.identity;
    production_requirement.task_id = production_task.task_id;
    production_requirement.turn_id = production_task.current_turn_id;
    production_requirement.content = "upgrade the public workflow API without breaking callers";
    conversation::TurnTaskLink production_link{production_task.identity,
        production_task.current_turn_id,production_task.task_id,
        production_task.current_run_id,1,conversation::TaskInputIntent::InitialRequest};
    assert(tasks->create(production_task,production_requirement,production_link).ok);
    run::RunCheckpoint production_run;
    production_run.metadata.identity.tenant_id = "tenant";
    production_run.metadata.identity.principal_id = "production-conversation";
    production_run.metadata.identity.task_id = "production-task";
    production_run.metadata.identity.run_id = "production-run";
    production_run.metadata.identity.plan_id = "production-task:plan";
    production_run.created_at = "2026-08-18T00:00:00Z";
    assert(durable_runs->create(production_run));
    lifetime.reset();
    harness::ProductionBuildReport assembly_report;
    assembly_report.ready = true;
    assembly_report.dependency_manifest_digest = "sha256:dependencies";
    assembly_report.composition_manifest_digest = "sha256:composition";
    assembly_report.deployment_manifest_digest = "sha256:deployment";
    assembly_report.startup_recovery.inspected = 2;
    assembly_report.startup_recovery.orphaned = 1;
    assembly_report.startup_recovery.queued_for_reconcile = 1;
    assembly_report.startup_timers_processed = 3;
    ProductionOwnershipReport assembly_ownership;
    assembly_ownership.ready = true;
    assembly_ownership.manifest_digest = "sha256:ownership";
    auto assembled = ProductionLiveRuntimeTestAccess::assemble(
        std::move(assembly_resources), std::move(assembly_harness),
        assembly_report, assembly_ownership);
    assert(assembled && assembled.runtime->task_control_service());
    const auto readiness = assembled.runtime->readiness_manifest();
    assert(readiness.at("production_ready").get<bool>());
    assert(readiness.at("response_executor").get<bool>());
    assert(readiness.at("long_task_executor").get<bool>());
    assert(readiness.at("session_run_executor").get<bool>());
    assert(readiness.at("startup_recovery").at("orphaned") == 1);
    assert(readiness.at("startup_timers_processed") == 3);
    assert(!readiness.at("canonical_digest").get<std::string>().empty());

    // Every native terminal producer enters through the production-owned
    // publisher.  The boundary becomes part of the durable idempotency key;
    // no subsystem is allowed to invent an independent Task transition.
    std::size_t observed_boundaries = 0;
    assembled.runtime->set_coordination_observer(
        [&](const recovery::CorrelatedStateEvent&,
            const recovery::TaskCoordinationDecision&) {
            ++observed_boundaries;
        });
    const recovery::CoordinationBoundary boundaries[] = {
        recovery::CoordinationBoundary::Conversation,
        recovery::CoordinationBoundary::Harness,
        recovery::CoordinationBoundary::Run,
        recovery::CoordinationBoundary::Invocation,
        recovery::CoordinationBoundary::Effect,
        recovery::CoordinationBoundary::Closure};
    for(std::size_t index = 0; index < std::size(boundaries); ++index) {
        recovery::CorrelatedStateEvent event;
        event.identity = {"tenant", "conversation-" + std::to_string(index)};
        event.task_id = "terminal-task-" + std::to_string(index);
        event.turn_id = "terminal-turn-" + std::to_string(index);
        event.run_id = "terminal-run-" + std::to_string(index);
        event.harness_id = "terminal-harness-" + std::to_string(index);
        event.task_revision = 1;
        event.turn_phase = conversation::TurnPhase::AwaitingInput;
        event.run_state = run::RunState::AwaitingApproval;
        event.harness_state = harness::HarnessState::AwaitingApproval;
        event.source_event_id = "native-terminal-1";
        conversation::PersistentTask task;
        task.identity = event.identity;
        task.task_id = event.task_id;
        task.root_turn_id = event.turn_id;
        task.current_turn_id = event.turn_id;
        task.current_run_id = event.run_id;
        conversation::TaskRequirementRevision requirement;
        requirement.identity = event.identity;
        requirement.task_id = event.task_id;
        requirement.turn_id = event.turn_id;
        requirement.content = "production terminal publication";
        conversation::TurnTaskLink link{event.identity, event.turn_id,
            event.task_id, event.run_id, 1,
            conversation::TaskInputIntent::InitialRequest};
        assert(tasks->create(task, requirement, link).ok);
        std::string publication_error;
        assert(assembled.runtime->publish_terminal_boundary(
            boundaries[index], event, &publication_error));
        assert(assembled.runtime->publish_terminal_boundary(
            boundaries[index], event, &publication_error));
        const auto command_id = event.task_id + ":" +
            std::string(recovery::name(boundaries[index])) +
            ":native-terminal-1";
        const auto command = journal->load(command_id);
        assert(command && command->state ==
            recovery::TaskCoordinationCommandState::Applied);
    }
    assert(observed_boundaries == std::size(boundaries) * 2);
    auto response = assembled.runtime->response_executor();
    auto long_task = assembled.runtime->long_task_executor();
    auto session_run = assembled.runtime->session_run_executor();
    {
    session::SQLiteSessionRunSupervisor production_supervisor(
        (root / "assembly-session-runs.sqlite3").string());
    session::SessionRunRequest supervised_request;
    supervised_request.tenant_id = "tenant";
    supervised_request.organization_id = "organization";
    supervised_request.project_id = "project";
    supervised_request.principal_id = "principal";
    supervised_request.provider_id = "provider";
    supervised_request.session_id = "production-session";
    supervised_request.run_id = "production-run";
    supervised_request.command_id = "start-production-run";
    supervised_request.payload = {{"conversation_id","production-conversation"},
        {"task_id","production-task"},{"turn_id","production-turn"},
        {"input","upgrade the public workflow API without breaking callers"},
        {"profile","professional"}};
    assert(production_supervisor.enqueue(std::move(supervised_request)).ok);
    session::SessionRunWorker production_worker(
        production_supervisor,"production-worker",5000,session_run);
    const auto now=static_cast<std::uint64_t>(std::chrono::duration_cast<
        std::chrono::milliseconds>(std::chrono::system_clock::now()
        .time_since_epoch()).count());
    const auto traversed = production_worker.tick(now);
    assert(traversed.claimed&&traversed.executed);
    assert(traversed.state == session::SupervisedRunState::Failed);
    assert(traversed.error == "execution_completed_without_deliverable");
    assert(assembly_counters->execute[harness::HarnessStage::PlanApproval] == 1);
    assert(assembly_counters->execute[harness::HarnessStage::Execution] == 1);
    assert(assembly_counters->execute[harness::HarnessStage::Assurance] == 1);
    }
    session::WorkerExecutionContext incomplete;
    incomplete.run.request.tenant_id = "tenant";
    incomplete.run.request.run_id = "missing-bindings";
    incomplete.heartbeat = [](std::uint64_t) { return true; };
    const auto rejected = session_run(incomplete);
    assert(rejected.disposition == session::WorkerDisposition::Failed);
    assert(rejected.diagnostic == "production_run_binding_incomplete");
    assembled.runtime.reset();
    assert(!lifetime_probe.expired());
    response = {};
    assert(!lifetime_probe.expired());
    long_task = {};
    session_run = {};
    assert(lifetime_probe.expired());
    std::filesystem::remove_all(root, ec);
}
