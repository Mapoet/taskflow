#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <filesystem>
#include <vector>

#include "agent/internal/platform_io.hpp"
#include "agent/ui/live_operations_projection.hpp"

int main() {
    using namespace agent_framework;
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("live-operations-" + std::to_string(internal::current_process_id()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    auto store = std::make_shared<SQLiteOperationsSnapshotStore>(
        (root / "operations.sqlite3").string());
    std::vector<Phase4OperationsSnapshot> published;
    LiveOperationsProjection projection(
        LiveOperationsIdentity{"tenant-live", "run-live", "task-live"}, store,
        [&](const auto& snapshot) { published.push_back(snapshot); });

    ToolExecutionEvent started;
    started.phase = ToolExecutionPhase::Started;
    started.tool_name = "filesystem.search";
    started.tool_call_id = "call-1";
    started.arguments = {{"secret", "must-not-be-projected"}};
    projection.observe_tool(started);
    auto running = projection.snapshot();
    assert(running.invocations.size() == 1);
    assert(running.invocations[0].status == OperationsStatus::Running);
    assert(running.source_revisions.back().revision == 1);

    ToolExecutionEvent completed = started;
    completed.phase = ToolExecutionPhase::Completed;
    completed.result = {{"value", 42}, {"credential", "must-not-be-projected"}};
    projection.observe_tool(completed);
    auto done = projection.snapshot();
    assert(done.invocations.size() == 1);
    assert(done.invocations[0].status == OperationsStatus::Passed);
    assert(done.source_revisions.back().revision == 2);
    assert(done.snapshot_id != running.snapshot_id);
    assert(published.size() == 2);
    projection.observe_task_actions({
        {"status", "task:read", true, "", 0},
        {"cancel", "task:control", false, "task_command_scope_forbidden:task:control", 0}},
        7);
    auto with_actions = projection.snapshot();
    assert(with_actions.task_actions.size() == 2);
    assert(with_actions.task_actions[0].expected_task_revision == 7);
    projection.observe_task_actions({{"continue", "task:write", true, "", 0}}, 6);
    assert(projection.snapshot().task_actions.size() == 2); // stale policy view rejected
    const auto action_roundtrip = Phase4OperationsProjection::from_json(
        Phase4OperationsProjection::to_json(with_actions));
    assert(action_roundtrip.task_actions.size() == 2);
    assert(!action_roundtrip.task_actions[1].enabled);
    const auto serialized = Phase4OperationsProjection::to_json(with_actions).dump();
    assert(serialized.find("must-not-be-projected") == std::string::npos);

    auto replay = store->latest("tenant-live", "run-live");
    assert(replay && replay->snapshot_id == with_actions.snapshot_id);
    LiveOperationsProjection recovered(*replay, store);
    ToolExecutionEvent failed = started;
    failed.tool_call_id = "call-2";
    failed.phase = ToolExecutionPhase::Completed;
    failed.result = {{"error", "redacted"}};
    recovered.observe_tool(failed);
    auto after_restart = recovered.snapshot();
    assert(after_restart.source_revisions.back().revision == 3);
    assert(after_restart.invocations.back().status == OperationsStatus::Failed);
    assert(after_restart.overall_status == OperationsStatus::Warning);
    tool_runtime::InvocationEvent queued;
    queued.invocation_id="durable-1";queued.event_type="queued";queued.sequence=1;
    queued.payload={{"tool_name","Bash"}};queued.event_digest="sha256:q";queued.created_at="2026-08-14T00:00:00Z";
    recovered.observe_invocation(queued);
    auto progress=queued;progress.event_type="progress";progress.sequence=2;
    progress.payload={{"tool_name","Bash"},{"fraction",0.5}};progress.event_digest="sha256:p";
    recovered.observe_invocation(progress);recovered.observe_invocation(progress);
    auto durable=recovered.snapshot();
    assert(durable.invocations.back().id=="durable-1"&&durable.summary.find("50.000000%")!=std::string::npos);
    auto completed_event=progress;completed_event.event_type="invocation_completed_candidate";completed_event.sequence=3;
    completed_event.event_digest="sha256:c";recovered.observe_invocation(completed_event);
    durable=recovered.snapshot();
    assert(durable.invocations.back().status==OperationsStatus::Passed);
    assert(durable.overall_status==OperationsStatus::Running); // a tool cannot close the task
    auto ambiguous=completed_event;ambiguous.event_type="not_verified";ambiguous.sequence=4;ambiguous.event_digest="sha256:u";
    recovered.observe_invocation(ambiguous);durable=recovered.snapshot();
    assert(durable.invocations.back().status==OperationsStatus::Unknown);
    assert(durable.overall_status==OperationsStatus::Warning);
    auto durable_source=std::find_if(durable.source_revisions.begin(),durable.source_revisions.end(),[](const auto& x){return x.store=="tool_invocation_events";});
    assert(durable_source!=durable.source_revisions.end()&&durable_source->revision==4);
    auto second=queued;second.invocation_id="durable-2";second.sequence=1;
    second.event_digest="sha256:q2";recovered.observe_invocation(second);
    durable=recovered.snapshot();
    assert(std::any_of(durable.invocations.begin(),durable.invocations.end(),
        [](const auto& value){return value.id=="durable-2";}));
    const auto durable_sources=std::count_if(durable.source_revisions.begin(),
        durable.source_revisions.end(),[](const auto& value){
            return value.store=="tool_invocation_events";});
    assert(durable_sources==2);
    conversation::RuntimeEventEnvelope cognition;
    cognition.event_id="harness:1";cognition.tenant_id="tenant-live";
    cognition.conversation_id="conversation-live";cognition.turn_id="turn-live";
    cognition.run_id="run-live";cognition.sequence=10;
    cognition.durability=conversation::EventDurability::Durable;
    cognition.event_type="harness.stage_result";cognition.timestamp="2026-08-15T00:00:00Z";
    cognition.payload={{"stage","cognition"},{"outcome","succeeded"},
        {"checkpoint_revision",4},{"output_digest","sha256:plan"},
        {"public_output",{{"schema","agent.lightweight_conversation_plan/v1"},
            {"acceptance_criteria",json::array({{{"id","criterion-1"}}})}}}};
    recovered.observe_runtime(cognition);
    auto harness=recovered.snapshot();
    assert(harness.plan_revision==1&&harness.criteria_total==1);
    assert(!harness.stages.empty()&&harness.stages.back().id=="cognition");
    auto waiting=cognition;waiting.sequence=11;waiting.payload={{"stage","execution"},
        {"outcome","awaiting_external"},{"checkpoint_revision",5}};
    recovered.observe_runtime(waiting);harness=recovered.snapshot();
    assert(harness.stages.back().status==OperationsStatus::Running);
    auto completed_harness=cognition;completed_harness.sequence=12;
    completed_harness.event_type="harness.harness_completed";
    completed_harness.payload=json::object();
    recovered.observe_runtime(completed_harness);
    harness=recovered.snapshot();
    assert(harness.task_closure_state=="execution_completed_unverified");
    assert(!harness.task_completion_verified&&harness.completion_authority=="none");
    recovered.observe_runtime(completed_harness);
    auto harness_source=std::find_if(harness.source_revisions.begin(),
        harness.source_revisions.end(),[](const auto& x){
            return x.store=="conversation_harness_events";
        });
    assert(harness_source!=harness.source_revisions.end()&&harness_source->revision==12);

    recovery::CorrelatedStateEvent coordination;
    coordination.identity = {"tenant-live", "conversation-live"};
    coordination.task_id = "task-live"; coordination.turn_id = "turn-live";
    coordination.run_id = "run-live"; coordination.harness_id = "harness-live";
    coordination.task_revision = 20; coordination.source_event_id = "closure:20";
    recovery::TaskCoordinationDecision closing;
    closing.command = recovery::CoordinationCommand::VerifyCompletion;
    closing.closure_state = "execution_completed_unverified";
    closing.reason_code = "semantic_verification_required";
    closing.digest = "sha256:coordination-20";
    recovered.observe_task_coordination(coordination, closing);
    auto coordinated = recovered.snapshot();
    assert(!coordinated.task_completion_verified);
    assert(coordinated.summary.find("verification pending") != std::string::npos);
    auto stale_coordination = coordination; stale_coordination.task_revision = 19;
    auto stale_decision = closing; stale_decision.digest = "sha256:stale";
    recovered.observe_task_coordination(stale_coordination, stale_decision);
    assert(recovered.snapshot().snapshot_id == coordinated.snapshot_id);
    coordination.task_revision = 21;
    recovery::TaskCoordinationDecision closed = closing;
    closed.command = recovery::CoordinationCommand::CloseVerified;
    closed.task_state = conversation::TaskLifecycleState::Closed;
    closed.closure_state = "completed_verified";
    closed.reason_code = "all_mandatory_criteria_verified";
    closed.terminal = true; closed.digest = "sha256:coordination-21";
    recovered.observe_task_coordination(coordination, closed);
    coordinated = recovered.snapshot();
    assert(coordinated.task_completion_verified);
    assert(coordinated.completion_authority == "task_closure_controller");
    assert(coordinated.overall_status == OperationsStatus::Passed);
    auto delivered = completed_harness;
    delivered.sequence = 13;
    delivered.event_type = "model_stop";
    delivered.payload = {{"reason","end_turn"},{"answer_present",true}};
    recovered.observe_runtime(delivered);
    coordinated = recovered.snapshot();
    assert(coordinated.response_delivery_state == "delivered");
    assert(coordinated.pipeline_state == "completed");
    assert(coordinated.task_completion_verified);
    assert(coordinated.completion_authority == "task_closure_controller");

    ToolExecutionEvent retained_active = started;
    retained_active.tool_call_id = "active-must-survive-retention";
    recovered.observe_tool(retained_active);
    for(int i = 0; i < 300; ++i) {
        ToolExecutionEvent terminal = started;
        terminal.tool_call_id = "retained-terminal-" + std::to_string(i);
        terminal.phase = ToolExecutionPhase::Completed;
        terminal.result = {{"ok", true}};
        recovered.observe_tool(terminal);
    }
    const auto bounded = recovered.snapshot();
    assert(bounded.invocations.size() <= Phase4OperationsProjection::max_items);
    assert(bounded.source_revisions.size() <= Phase4OperationsProjection::max_items);
    assert(bounded.invocations_compacted > 0);
    assert(std::any_of(bounded.invocations.begin(), bounded.invocations.end(), [](const auto& item) {
        return item.id == "active-must-survive-retention" &&
               item.status == OperationsStatus::Running;
    }));
    const auto bounded_replay = store->latest("tenant-live", "run-live");
    assert(bounded_replay && bounded_replay->invocations.size() <=
           Phase4OperationsProjection::max_items);
    assert(bounded_replay->invocations_compacted == bounded.invocations_compacted);
    fs::remove_all(root, ec);
    return 0;
}
