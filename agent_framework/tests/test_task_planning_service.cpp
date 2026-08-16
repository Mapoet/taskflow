#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <filesystem>

#include "agent/conversation/task_planning_service.hpp"
#include "agent/internal/platform_io.hpp"
#include "phase4_cognition_pipeline_test_support.hpp"

int main() {
    using namespace agent_framework;
    using namespace agent_framework::conversation;
    using namespace agent_framework::planning;
    using namespace phase4_cognition_test;
    const auto root = std::filesystem::temp_directory_path() /
        ("task-planning-service-" + std::to_string(
            internal::current_process_id()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    SQLiteTaskRegistry tasks((root / "conversation.sqlite3").string());
    harness::SQLiteProductionWorkflowInputRepository inputs(
        (root / "inputs.sqlite3").string());
    const ConversationIdentity identity{"tenant-a", "user-a"};
    PersistentTask task;
    task.identity = identity;
    task.task_id = "task-planning";
    task.root_turn_id = "turn-1";
    task.current_turn_id = "turn-1";
    task.current_run_id = "run-1";
    TaskRequirementRevision requirement;
    requirement.identity = identity;
    requirement.task_id = task.task_id;
    requirement.turn_id = "turn-1";
    requirement.content = "upgrade the public workflow API without breaking callers";
    TurnTaskLink link{identity, "turn-1", task.task_id, "run-1", 1,
                      TaskInputIntent::InitialRequest};
    assert(tasks.create(task, requirement, link).ok);
    task = *tasks.load(identity, task.task_id);

    memory_v2::MemoryProviderRegistry providers;
    memory_v2::MemoryViewEngine views(providers);
    InvestigatorRegistry investigators;
    assert(investigators.register_investigator(std::make_shared<RepoInvestigator>()));
    assert(investigators.register_investigator(std::make_shared<DocsInvestigator>()));
    InMemoryEvidenceStore evidence;
    InMemoryPlanStore plans;
    InMemoryCognitionCheckpointStore checkpoints;
    ScriptedStageModel model;
    script_success(model, false);
    MultiStageCognitionWorkflow cognition(
        views, investigators, evidence, plans, checkpoints, model);
    TaskPlanningService service(tasks, cognition, inputs);
    TaskPlanningPolicy policy;
    policy.requested_deliverables = {"code", "tests", "migration notes"};
    policy.granted_authorities = {
        "workspace_read", "workspace_write", "repo_read", "repo_write", "external_read"};
    policy.success_signals = {"all tests pass", "public callers migrated"};
    policy.executor_id = "workspace-agent";
    policy.executor_revision = "r1";
    const auto missing_acceptance = service.plan(task, policy);
    assert(missing_acceptance.state == TaskPlanningState::Failed);
    assert(missing_acceptance.error_code ==
           "production_acceptance_contract_required");
    assert(model.requests.empty());
    assurance::AcceptanceContract acceptance;
    acceptance.revision = 1;
    acceptance.criteria = {{"public-api-compatible",
        assurance::VerificationLayer::Integration,
        "public callers remain compatible", "test", {"test-report"}, "pass", true}};
    policy.acceptance_contract = acceptance;
    auto planned = service.plan(task, policy);
    assert(planned.state == TaskPlanningState::Planned);
    assert(planned.plan_revision == 1 && !planned.plan_digest.empty());
    assert(!planned.context_projection_digest.empty());
    assert(!planned.acceptance_contract_digest.empty());
    auto stored = tasks.load(identity, task.task_id);
    assert(stored && stored->plan_revision == 1);
    auto run_links = tasks.runs(identity, task.task_id);
    assert(run_links.size() == 1);
    assert(run_links[0].requirement_revision == 1);
    assert(run_links[0].plan_digest == planned.plan_digest);
    contracts::ContractIdentity scope;
    scope.tenant_id = identity.tenant_id;
    scope.principal_id = identity.conversation_id;
    scope.task_id = task.task_id;
    scope.run_id = "run-1";
    scope.plan_id = task.task_id + ":plan";
    assert(inputs.intake(scope));
    const auto task_context = inputs.task_context(scope, planned.plan_digest);
    assert(task_context);
    assert(task_context->at("context_projection_digest") ==
           planned.context_projection_digest);
    assert(task_context->at("context_projection").at("segments").size() == 4);
    assert(task_context->at("acceptance_contract_digest") ==
           planned.acceptance_contract_digest);
    assert(inputs.acceptance_contract(scope, planned.acceptance_contract_digest));
    assert(inputs.descriptor(scope, planned.plan_digest, "inspect"));
    assert(inputs.descriptor(scope, planned.plan_digest, "change"));
    const auto calls = model.requests.size();
    const auto replay = service.plan(*stored, policy);
    assert(replay.state == TaskPlanningState::Planned);
    assert(replay.plan_digest == planned.plan_digest);
    assert(replay.context_projection_digest == planned.context_projection_digest);
    assert(model.requests.size() == calls);

    // A process restart must recover the durable binding without invoking the LLM again.
    SQLiteTaskRegistry restarted_tasks((root / "conversation.sqlite3").string());
    harness::SQLiteProductionWorkflowInputRepository restarted_inputs(
        (root / "inputs.sqlite3").string());
    ScriptedStageModel restart_model;
    MultiStageCognitionWorkflow restarted_cognition(
        views, investigators, evidence, plans, checkpoints, restart_model);
    TaskPlanningService restarted_service(
        restarted_tasks, restarted_cognition, restarted_inputs);
    const auto restarted_task = restarted_tasks.load(identity, task.task_id);
    assert(restarted_task);
    const auto recovered = restarted_service.plan(*restarted_task, policy);
    assert(recovered.state == TaskPlanningState::Planned);
    assert(recovered.plan_digest == planned.plan_digest);
    assert(recovered.context_projection_digest == planned.context_projection_digest);
    assert(restart_model.requests.empty());

    // Concurrent amendments use task revision CAS. Exactly one revision becomes canonical.
    TaskRequirementRevision amendment{identity, task.task_id, 0,
        TaskInputIntent::AmendRequirements, "turn-2",
        "also preserve binary compatibility for existing plugins"};
    TurnTaskLink amendment_link{identity, "turn-2", task.task_id, "run-2", 0,
                                TaskInputIntent::AmendRequirements};
    const auto amended = restarted_tasks.append_requirement(
        amendment, amendment_link, restarted_task->revision, "run-2");
    assert(amended.ok);
    TaskRequirementRevision loser = amendment;
    loser.turn_id = "turn-3";
    loser.content = "a stale concurrent amendment";
    TurnTaskLink loser_link{identity, "turn-3", task.task_id, "run-3", 0,
                            TaskInputIntent::AmendRequirements};
    const auto rejected = restarted_tasks.append_requirement(
        loser, loser_link, restarted_task->revision, "run-3");
    assert(!rejected.ok && rejected.error == "task_revision_conflict");

    // The winning requirement revision gets a new run binding and superseding plan.
    script_success(restart_model, false);
    const auto amended_task = restarted_tasks.load(identity, task.task_id);
    assert(amended_task && amended_task->requirement_revision == 2);
    const auto superseded = restarted_service.plan(*amended_task, policy);
    assert(superseded.state == TaskPlanningState::Planned);
    assert(superseded.plan_revision == 2);
    assert(superseded.plan_digest != planned.plan_digest);
    assert(!superseded.context_projection_digest.empty());
    assert(superseded.context_projection_digest != planned.context_projection_digest);
    const auto amended_runs = restarted_tasks.runs(identity, task.task_id);
    assert(amended_runs.size() == 2);
    assert(amended_runs[1].run_id == "run-2");
    assert(amended_runs[1].requirement_revision == 2);
    assert(amended_runs[1].plan_revision == 2);
    assert(amended_runs[1].plan_digest == superseded.plan_digest);

    std::filesystem::remove_all(root, error);
}
