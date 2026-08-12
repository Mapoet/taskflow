#include <agent/conversation/conversation_engine.hpp>
#include <agent/conversation/production_bridge.hpp>
#include <agent/conversation/context_projection.hpp>
#include <agent/conversation/graph_turn_adapter.hpp>
#include <agent/internal/platform_io.hpp>
#include "phase4_harness_test_support.hpp"
#include <cassert>
#include <filesystem>
#include <thread>
using namespace agent_framework::conversation;
int main()
{
    auto path = std::filesystem::temp_directory_path() / ("conversation-" + std::to_string(agent_framework::internal::current_process_id()) + ".sqlite3");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    ContextProjectionManifest projection;
    projection.identity = {"tenant", "conversation"};
    projection.turn_id = "turn";
    projection.revision = 1;
    projection.profile_revision_digest = "profile";
    projection.prompt_revision_digest = "prompt";
    projection.segments = {
        {"contract", "cas://contract", "sha256:c", "task_closure_controller", "", 100, true},
        {"policy", "cas://policy", "sha256:p", "pdp", "", 50, true},
        {"citation", "cas://citation", "sha256:x", "source", "", 50, true}};
    auto projected = ContextProjector::build(projection);
    assert(projected && !projected->digest.empty());
    CompactBoundaryRecord boundary;
    boundary.identity = projection.identity;
    boundary.turn_id = "turn";
    boundary.revision = 1;
    boundary.summary_digest = "sha256:s";
    boundary.pre_tokens = 1000;
    boundary.post_tokens = 400;
    assert(ContextProjector::validate_boundary(*projected, boundary));
    projected->segments[0].truncation_reason = "budget";
    assert(!ContextProjector::validate_boundary(*projected, boundary));
    {
        SQLiteConversationStore store(path.string());
        int calls = 0;
        ConversationEngine engine(store, [&](const TurnRequest &, const TurnCheckpoint &)
                                  {++calls;ModelTurnOutcome o;o.reason=calls==1?ModelTurnStopReason::ToolRequested:ModelTurnStopReason::EndTurn;o.tool_receipt_refs=calls==1?std::vector<std::string>{"receipt"}:std::vector<std::string>{};o.candidate_answer=calls==1?"calling":"candidate";return o; });
        TurnRequest r{{"tenant", "conversation"}, "turn-1", "hello", TaskExecutionProfile::ReadOnlyAnalysis, 10};
        auto first = engine.start_turn(r);
        assert(first.error.empty() && first.checkpoint.phase == TurnPhase::AwaitingTool && !first.outcome.task_completion_verified);
        auto second = engine.continue_turn(r, TurnContinuationReason::ToolResultsAvailable);
        assert(second.error.empty() && second.checkpoint.phase == TurnPhase::Completed && !second.outcome.task_completion_verified);
        auto messages = store.messages(r.identity);
        assert(messages.size() == 3 && messages[1].parent_id == messages[0].message_id);
        auto events = store.events(r.identity);
        assert(events.size() == 3 && events.back().sequence == 3);
        assert(!engine.start_turn(r).error.empty());
        assert(engine.classify_input("/status") == InputDisposition::StatusQuery);

        ConversationEngine input_engine(store, [](const TurnRequest &, const TurnCheckpoint &) {
            ModelTurnOutcome outcome; outcome.reason = ModelTurnStopReason::ToolRequested;
            outcome.tool_receipt_refs = {"pending-tool"}; return outcome;
        });
        TurnRequest input_turn{{"tenant", "input-routing"}, "turn-input", "start",
                               TaskExecutionProfile::Conversation, 10};
        assert(input_engine.start_turn(input_turn).error.empty());
        input_turn.input = "additional context";
        assert(input_engine.submit_user_input(input_turn, InputDisposition::AppendToCurrentTurn));
        assert(store.inputs(input_turn.identity, InputState::Consumed).size() == 1);
        input_turn.input = "/next next task";
        assert(input_engine.submit_user_input(input_turn, InputDisposition::QueueNextTurn));
        input_turn.input = "/replace replacement task";
        assert(input_engine.submit_user_input(input_turn, InputDisposition::InterruptAndReplace));
        auto queued_inputs = store.inputs(input_turn.identity, InputState::Queued);
        assert(queued_inputs.size() == 2);
        assert(queued_inputs[0].disposition == InputDisposition::QueueNextTurn);
        assert(queued_inputs[0].content == "next task");
        assert(queued_inputs[1].disposition == InputDisposition::InterruptAndReplace);
        assert(queued_inputs[1].content == "replacement task");
        input_turn.input = "/next ";
        assert(!input_engine.submit_user_input(input_turn, InputDisposition::QueueNextTurn));
        auto interrupted = store.load_turn(input_turn.identity, input_turn.turn_id);
        assert(interrupted && interrupted->phase == TurnPhase::Interrupted);
        assert(store.messages(input_turn.identity).size() == 2);
    }
    {
        SQLiteConversationStore reopened(path.string());
        auto m = reopened.messages({"tenant", "conversation"});
        assert(m.size() == 3);
        auto c = reopened.load_turn({"tenant", "conversation"}, "turn-1");
        assert(c && c->phase == TurnPhase::Completed);
        ConversationIdentity input_identity{"tenant", "input-routing"};
        assert(reopened.inputs(input_identity, InputState::Queued).size() == 2);
        ConversationEngine restarted(reopened, [](const TurnRequest &request, const TurnCheckpoint &) {
            assert(request.profile == TaskExecutionProfile::Conversation);
            assert(request.max_iterations == 10);
            ModelTurnOutcome outcome; outcome.reason = ModelTurnStopReason::EndTurn;
            outcome.candidate_answer = "queued complete"; return outcome;
        });
        auto drained = restarted.drain_queued_turns(input_identity, 10);
        assert(drained.size() == 2);
        assert(drained[0].error.empty() && drained[1].error.empty());
        assert(drained[0].checkpoint.turn_id == "turn-input:input:4:turn");
        assert(drained[1].checkpoint.turn_id == "turn-input:input:5:turn");
        assert(reopened.inputs(input_identity, InputState::Queued).empty());
        auto consumed = reopened.inputs(input_identity, InputState::Consumed);
        assert(consumed.size() == 3);
        assert(consumed[1].consumed_turn_id == drained[0].checkpoint.turn_id);
        assert(consumed[2].consumed_turn_id == drained[1].checkpoint.turn_id);
        assert(restarted.start_next_queued_turn(input_identity).error == "no_queued_input");
        auto queued_messages = reopened.messages(input_identity);
        assert(queued_messages.size() == 6);
        for (std::size_t i = 1; i < queued_messages.size(); ++i)
            assert(queued_messages[i].parent_id == queued_messages[i - 1].message_id);
        auto queued_events = reopened.events(input_identity);
        assert(queued_events.size() == 15);
        assert(queued_events[5].event_type == "user_input_claimed");
        assert(queued_events[6].event_type == "user_input_consumed");
        assert(queued_events[7].event_type == "turn_created_from_queued_input");
        assert(queued_events[8].event_type == "turn_started");
    }
    std::filesystem::remove(path, ec);
    ModelTurnOutcome invalid;
    invalid.task_completion_verified = true;
    assert(!validate(invalid).empty() && !outcome_can_close_task(invalid));
    assert(!task_execution_profile("unknown"));
    assert(!TaskProfileRouter::route(TaskExecutionProfile::Conversation, true, true).allowed);
    agent_framework::assurance::AcceptanceContract a;
    a.metadata = phase4_harness_test::metadata("conversation-contract");
    a.criteria = {{"exists", agent_framework::assurance::VerificationLayer::Functional, "exists", "artifact", {}, "", true}};
    std::string error;
    auto c = closure_contract_from(a, TaskExecutionProfile::ArtifactDelivery, &error);
    assert(c && c->mandatory_criteria.size() == 1);
    agent_framework::ExecutionResult execution;
    execution.success = true;
    execution.status = agent_framework::ExecutionTerminalStatus::Completed;
    execution.outputs = {{"final_answer", "candidate"},
                         {"model_stop_reason", "model_turn_completed"},
                         {"task_completion_verified", true}};
    auto turn = GraphTurnAdapter::from_execution(execution);
    assert(turn.reason == ModelTurnStopReason::EndTurn);
    assert(!turn.task_completion_verified && turn.candidate_answer == "candidate");
    agent_framework::WorkflowResult workflow{};
    workflow.success = false;
    workflow.outputs = {{"final_answer", "candidate failure"}};
    workflow.error_message = "provider unavailable";
    auto workflow_turn = GraphTurnAdapter::from_workflow(workflow);
    assert(workflow_turn.reason == ModelTurnStopReason::ProviderError);
    assert(!workflow_turn.task_completion_verified && workflow_turn.candidate_answer == "candidate failure");
    workflow.success = true; workflow.error_message.reset(); workflow.exit_code = 4;
    workflow.outputs = {{"model_stop_reason", "guard_stopped"}};
    assert(GraphTurnAdapter::from_workflow(workflow).reason == ModelTurnStopReason::GuardStopped);
    auto long_path = std::filesystem::temp_directory_path() /
        ("conversation-100-" + std::to_string(agent_framework::internal::current_process_id()) + ".sqlite3");
    std::filesystem::remove(long_path, ec);
    {
        SQLiteConversationStore long_store(long_path.string());
        ConversationEngine long_engine(long_store, [](const TurnRequest&, const TurnCheckpoint&) {
            ModelTurnOutcome out; out.reason=ModelTurnStopReason::EndTurn;
            out.candidate_answer="ok"; return out;
        });
        for(int i=0;i<100;++i) {
            TurnRequest request{{"tenant","long"},"turn-"+std::to_string(i),
                "input-"+std::to_string(i),TaskExecutionProfile::Conversation,1};
            assert(long_engine.start_turn(request).error.empty());
        }
        auto chain=long_store.messages({"tenant","long"});
        assert(chain.size()==200);
        for(std::size_t i=1;i<chain.size();++i)
            assert(chain[i].parent_id==chain[i-1].message_id);
    }
    { SQLiteConversationStore reopened(long_path.string());
      assert(reopened.messages({"tenant","long"}).size()==200); }
    std::filesystem::remove(long_path, ec);

    auto race_path = std::filesystem::temp_directory_path() /
        ("conversation-race-" + std::to_string(agent_framework::internal::current_process_id()) + ".sqlite3");
    std::filesystem::remove(race_path, ec);

    auto claim_path = std::filesystem::temp_directory_path() /
        ("conversation-claim-" + std::to_string(agent_framework::internal::current_process_id()) + ".sqlite3");
    std::filesystem::remove(claim_path, ec);
    SQLiteConversationStore claim_seed_store(claim_path.string());
    ConversationEngine claim_seed_engine(claim_seed_store,
        [](const TurnRequest &, const TurnCheckpoint &) {
            ModelTurnOutcome outcome; outcome.reason = ModelTurnStopReason::ToolRequested;
            outcome.tool_receipt_refs = {"pending"}; return outcome;
        });
    TurnRequest claim_seed{{"tenant", "claim-race"}, "claim-seed", "seed",
                           TaskExecutionProfile::Conversation, 2};
    assert(claim_seed_engine.start_turn(claim_seed).error.empty());
    claim_seed.input = "race payload";
    assert(claim_seed_engine.submit_user_input(claim_seed, InputDisposition::QueueNextTurn));
    SQLiteConversationStore claim_store_a(claim_path.string());
    SQLiteConversationStore claim_store_b(claim_path.string());
    std::optional<QueuedTurnClaim> claim_a, claim_b;
    std::thread claimer_a([&] { claim_a = claim_store_a.consume_next_queued_input(claim_seed.identity); });
    std::thread claimer_b([&] { claim_b = claim_store_b.consume_next_queued_input(claim_seed.identity); });
    claimer_a.join(); claimer_b.join();
    assert(claim_a.has_value() != claim_b.has_value());
    assert(claim_seed_store.inputs(claim_seed.identity, InputState::Queued).empty());
    assert(claim_seed_store.inputs(claim_seed.identity, InputState::Consumed).size() == 1);
    assert(claim_seed_store.messages(claim_seed.identity).size() == 2);
    assert(claim_seed_store.events(claim_seed.identity).size() == 7);
    std::filesystem::remove(claim_path, ec);
    SQLiteConversationStore first_store(race_path.string());
    SQLiteConversationStore second_store(race_path.string());
    ConversationIdentity race_identity{"tenant", "race"};
    TurnCheckpoint seed_checkpoint{race_identity, "seed"};
    seed_checkpoint.revision = 1;
    seed_checkpoint.phase = TurnPhase::Completed;
    ConversationMessage seed_message{race_identity, "seed-message", "", "seed", "user", "seed", "now", 0, ""};
    RuntimeEventEnvelope seed_event;
    seed_event.turn_id = "seed"; seed_event.run_id = "seed";
    seed_event.event_type = "seeded"; seed_event.timestamp = "now";
    seed_event.durability = EventDurability::Durable;
    ConversationCommit seed{seed_checkpoint, 0, {seed_message}, {seed_event}};
    assert(first_store.commit(seed));

    auto make_race_commit = [&](std::string id) {
        TurnCheckpoint checkpoint{race_identity, id};
        checkpoint.revision = 1; checkpoint.phase = TurnPhase::Completed;
        ConversationMessage message{race_identity, id + "-message", "seed-message", id,
                                    "user", id, "now", 0, ""};
        RuntimeEventEnvelope event;
        event.turn_id = id; event.run_id = id; event.event_type = "race";
        event.timestamp = "now"; event.durability = EventDurability::Durable;
        return ConversationCommit{checkpoint, 0, {message}, {event}};
    };
    auto race_a = make_race_commit("race-a");
    auto race_b = make_race_commit("race-b");
    bool race_a_ok = false, race_b_ok = false;
    std::thread writer_a([&] { race_a_ok = first_store.commit(race_a); });
    std::thread writer_b([&] { race_b_ok = second_store.commit(race_b); });
    writer_a.join(); writer_b.join();
    assert(race_a_ok != race_b_ok);
    assert(first_store.messages(race_identity).size() == 2);
    auto race_events = first_store.events(race_identity);
    assert(race_events.size() == 2 && race_events[0].sequence == 1 && race_events[1].sequence == 2);

    auto rollback = make_race_commit("rollback");
    rollback.messages.front().parent_id = "wrong-parent";
    assert(!first_store.commit(rollback));
    assert(!first_store.load_turn(race_identity, "rollback"));
    assert(first_store.messages(race_identity).size() == 2);
    assert(first_store.events(race_identity).size() == 2);
    std::filesystem::remove(race_path, ec);

    auto stream_path = std::filesystem::temp_directory_path() /
        ("conversation-stream-" + std::to_string(agent_framework::internal::current_process_id()) + ".sqlite3");
    std::filesystem::remove(stream_path, ec);
    {
        SQLiteConversationStore stream_store(stream_path.string());
        EventStreamHub hub(stream_store);
        ConversationEngine stream_engine(stream_store,
            [](const TurnRequest &, const TurnCheckpoint &) {
                ModelTurnOutcome outcome; outcome.reason = ModelTurnStopReason::EndTurn;
                outcome.candidate_answer = "streamed"; return outcome;
            }, {}, &hub);
        ConversationIdentity stream_identity{"tenant", "event-stream"};
        auto live = stream_engine.subscribe_events(stream_identity, 0, 8);
        assert(live.subscription && live.head_sequence == 0 && live.error.empty());
        TurnRequest stream_turn{stream_identity, "stream-turn", "hello",
                                TaskExecutionProfile::Conversation, 2};
        assert(stream_engine.start_turn(stream_turn).error.empty());
        RuntimeEventEnvelope delivered;
        assert(live.subscription->next(delivered, std::chrono::milliseconds(20)) ==
               SubscriptionRead::Event && delivered.sequence == 1);
        assert(live.subscription->next(delivered, std::chrono::milliseconds(20)) ==
               SubscriptionRead::Event && delivered.sequence == 2);
        assert(live.subscription->cursor() == 2);
        assert(stream_store.event_retention_floor(stream_identity) == 1);
        auto resumed = stream_engine.subscribe_events(stream_identity, 1, 8);
        assert(resumed.subscription && resumed.head_sequence == 2);
        assert(resumed.subscription->next(delivered, std::chrono::milliseconds(20)) ==
               SubscriptionRead::Event && delivered.sequence == 2);
        assert(!stream_engine.subscribe_events(stream_identity, 3, 8).subscription);
        assert(stream_engine.subscribe_events(stream_identity, 0, 1).error ==
               "replay_exceeds_capacity");
        auto slow = stream_engine.subscribe_events(stream_identity, 2, 1);
        assert(slow.subscription);
        TurnRequest second_turn{stream_identity, "stream-turn-2", "again",
                                TaskExecutionProfile::Conversation, 2};
        assert(stream_engine.start_turn(second_turn).error.empty());
        assert(slow.subscription->overflowed());
        assert(slow.subscription->next(delivered, std::chrono::milliseconds(20)) ==
               SubscriptionRead::Event && delivered.sequence == 3);
        assert(slow.subscription->next(delivered, std::chrono::milliseconds(20)) ==
               SubscriptionRead::Overflow);
        hub.close_all();
    }
    {
        SQLiteConversationStore reopened(stream_path.string());
        EventStreamHub hub(reopened);
        auto replay = hub.subscribe({"tenant", "event-stream"}, 2, 8);
        assert(replay.subscription && replay.head_sequence == 4);
        RuntimeEventEnvelope event;
        assert(replay.subscription->next(event, std::chrono::milliseconds(20)) ==
               SubscriptionRead::Event && event.sequence == 3);
        assert(replay.subscription->next(event, std::chrono::milliseconds(20)) ==
               SubscriptionRead::Event && event.sequence == 4);
    }
    std::filesystem::remove(stream_path, ec);

    RuntimeEventEnvelope decoded_source;
    decoded_source.event_id = "decoded:1"; decoded_source.tenant_id = "tenant";
    decoded_source.conversation_id = "decode"; decoded_source.turn_id = "turn";
    decoded_source.run_id = "run"; decoded_source.sequence = 1;
    decoded_source.durability = EventDurability::Durable;
    decoded_source.event_type = "decoded"; decoded_source.timestamp = "now";
    auto decoded = decode_runtime_event(encode(decoded_source));
    assert(decoded && decoded->sequence == 1 && decoded->event_type == "decoded");
    auto invalid_runtime_json = encode(decoded_source);
    invalid_runtime_json["schema"] = "agent.runtime_event/v999";
    assert(!decode_runtime_event(invalid_runtime_json));
}
