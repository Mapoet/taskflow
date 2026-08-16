#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <filesystem>

#include "agent/recovery/task_state_coordinator.hpp"
#include "agent/internal/platform_io.hpp"

int main() {
    using namespace agent_framework;
    recovery::TaskStateCoordinator coordinator;
    recovery::CorrelatedStateEvent event;
    event.identity = {"tenant-a", "conversation-a"};
    event.task_id = "task-a"; event.turn_id = "turn-a";
    event.run_id = "run-a"; event.harness_id = "turn:turn-a";
    event.task_revision = 4; event.source_event_id = "event-1";
    event.turn_phase = conversation::TurnPhase::Failed;
    event.run_state = run::RunState::Running;
    event.harness_state = harness::HarnessState::Running;
    auto decision = coordinator.observe(event);
    assert(decision.command == recovery::CoordinationCommand::ContinueExecution);
    assert(!decision.terminal && !decision.digest.empty());

    event.run_state = run::RunState::Completed;
    event.harness_state = harness::HarnessState::Completed;
    event.turn_phase = conversation::TurnPhase::Completed;
    decision = coordinator.observe(event);
    assert(decision.command == recovery::CoordinationCommand::VerifyCompletion);
    assert(decision.closure_state == "execution_completed_unverified");

    event.closure_verified = true;
    decision = coordinator.observe(event);
    assert(decision.command == recovery::CoordinationCommand::CloseVerified);
    assert(decision.terminal);

    event.pending_effect = true;
    decision = coordinator.observe(event);
    assert(decision.command == recovery::CoordinationCommand::ContinueExecution);
    assert(!decision.terminal);
    event.pending_effect = false;
    event.unknown_effect = true;
    decision = coordinator.observe(event);
    assert(decision.command == recovery::CoordinationCommand::ManualReview);

    event.unknown_effect = false;
    event.turn_phase = conversation::TurnPhase::AwaitingInput;
    event.run_state = run::RunState::AwaitingApproval;
    event.harness_state = harness::HarnessState::AwaitingApproval;
    decision = coordinator.observe(event);
    assert(decision.command == recovery::CoordinationCommand::AwaitApproval);
    assert(decision.task_state == conversation::TaskLifecycleState::AwaitingApproval);

    // Simulate a crash after durable publication but before applying the Task CAS.
    const auto root = std::filesystem::temp_directory_path() /
        ("task-state-coordinator-" + std::to_string(internal::current_process_id()));
    std::error_code error; std::filesystem::remove_all(root,error);
    std::filesystem::create_directories(root);
    conversation::SQLiteTaskRegistry tasks((root/"tasks.sqlite3").string());
    conversation::PersistentTask task; task.identity=event.identity; task.task_id=event.task_id;
    task.root_turn_id=event.turn_id; task.current_turn_id=event.turn_id; task.current_run_id=event.run_id;
    conversation::TaskRequirementRevision requirement; requirement.identity=event.identity;
    requirement.task_id=event.task_id; requirement.turn_id=event.turn_id; requirement.content="test";
    conversation::TurnTaskLink link{event.identity,event.turn_id,event.task_id,event.run_id,1,
                                    conversation::TaskInputIntent::InitialRequest};
    assert(tasks.create(task,requirement,link).ok);
    event.task_revision=1; event.source_event_id="event-crash";
    event.turn_phase=conversation::TurnPhase::AwaitingInput;
    event.run_state=run::RunState::AwaitingApproval;
    event.harness_state=harness::HarnessState::AwaitingApproval;
    const auto durable_decision=coordinator.observe(event);
    {
        recovery::SQLiteTaskCoordinationJournal before((root/"coordination.sqlite3").string());
        recovery::DurableTaskCoordinationCommand command;
        command.command_id=event.task_id+":"+event.source_event_id;
        command.event=event; command.decision=durable_decision;
        assert(before.submit(command));
        assert(before.pending(10).size()==1);
    }
    {
        recovery::SQLiteTaskCoordinationJournal after((root/"coordination.sqlite3").string());
        recovery::DurableTaskStateCoordinator durable(tasks,after);
        assert(durable.reconcile_pending(10)==1);
        assert(durable.reconcile(event.task_id+":"+event.source_event_id));
        const auto applied=tasks.load(event.identity,event.task_id);
        assert(applied&&applied->state==conversation::TaskLifecycleState::AwaitingApproval);
        assert(applied->closure_state=="awaiting_approval");
        assert(after.pending(10).empty());

        // A late command based on the pre-apply Task revision is quarantined.
        auto stale_event=event; stale_event.source_event_id="event-stale";
        stale_event.turn_phase=conversation::TurnPhase::Completed;
        stale_event.run_state=run::RunState::Completed;
        stale_event.harness_state=harness::HarnessState::Completed;
        stale_event.closure_verified=true;
        const auto stale_decision=coordinator.observe(stale_event);
        std::string conflict;
        assert(!durable.publish(stale_event,stale_decision,&conflict));
        assert(conflict=="task_revision_conflict");
        const auto quarantined=after.load(stale_event.task_id+":"+stale_event.source_event_id);
        assert(quarantined&&quarantined->state==recovery::TaskCoordinationCommandState::Conflict);
    }
    std::filesystem::remove_all(root,error);
}
