#include <agent/conversation/store.hpp>
#include <agent/harness/store.hpp>
#include <agent/recovery/system_state_reconciler.hpp>
#include <agent/internal/platform_io.hpp>
#include <agent/tool_runtime/store.hpp>
#include <agent/toolbus/tool_effect_journal.hpp>

#include <cassert>
#include <algorithm>
#include <filesystem>
#include <stdexcept>

#undef assert
#define assert(condition)                                                        \
    do {                                                                         \
        if (!(condition))                                                        \
            throw std::runtime_error("check failed: " #condition);              \
    } while (false)

using namespace agent_framework;

namespace {

harness::HarnessEvent event_for(const harness::HarnessCheckpoint& checkpoint,
                                std::string type) {
    harness::HarnessEvent event;
    event.harness_id = checkpoint.harness_id;
    event.sequence = checkpoint.revision;
    event.checkpoint_revision = checkpoint.revision;
    event.event_type = std::move(type);
    event.payload = {{"revision", checkpoint.revision}};
    event.created_at = "1000";
    return event;
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("system-state-reconciler-" +
         std::to_string(internal::current_process_id()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);

    conversation::SQLiteConversationStore conversations(
        (root / "conversation.sqlite3").string());
    harness::InMemoryHarnessStore harnesses;
    const conversation::ConversationIdentity identity{"tenant", "conversation"};

    conversation::TurnCheckpoint failed;
    failed.identity = identity;
    failed.turn_id = "turn-failed";
    failed.revision = 1;
    failed.iteration = 1;
    failed.phase = conversation::TurnPhase::Failed;
    failed.continuation = conversation::TurnContinuationReason::None;
    assert(conversations.commit_turn(failed, 0, nullptr));

    conversation::TurnCheckpoint never_started;
    never_started.identity = identity;
    never_started.turn_id = "turn-zero";
    never_started.revision = 1;
    never_started.phase = conversation::TurnPhase::Running;
    assert(conversations.commit_turn(never_started, 0, nullptr));

    harness::HarnessCheckpoint checkpoint;
    checkpoint.metadata.identity.tenant_id = identity.tenant_id;
    checkpoint.metadata.identity.task_id = "task";
    checkpoint.metadata.extensions["conversation_id"] = identity.conversation_id;
    checkpoint.harness_id = "turn:" + failed.turn_id;
    checkpoint.revision = 1;
    checkpoint.state = harness::HarnessState::Running;
    checkpoint.next_stage = harness::HarnessStage::Execution;
    checkpoint.updated_at = "1000";
    harness::HarnessOutboxEntry pending;
    pending.effect_id = "effect";
    pending.stage = harness::HarnessStage::Execution;
    pending.state = harness::OutboxState::Pending;
    checkpoint.outbox.push_back(pending);
    assert(harnesses.create(checkpoint, event_for(checkpoint, "created")));

    recovery::SystemStateReconciler reconciler(conversations, harnesses);
    const recovery::ReconciliationScope scope{identity, 100};
    const auto first = reconciler.scan(scope);
    const auto second = reconciler.scan(scope);
    assert(!first.digest.empty());
    assert(first.digest == second.digest);
    assert(first.findings.size() == 2);

    const auto dry = reconciler.apply(first, true);
    assert(dry.inspected == 2);
    assert(dry.changed == 0);
    assert(dry.manual_review == 2);
    assert(harnesses.load(identity.tenant_id, checkpoint.harness_id)->checkpoint.state ==
           harness::HarnessState::Running);

    const auto applied = reconciler.apply(first, false);
    assert(applied.changed == 1);
    assert(applied.conflicts == 0);
    assert(applied.errors.empty());
    const auto recovered = harnesses.load(identity.tenant_id, checkpoint.harness_id);
    assert(recovered);
    assert(recovered->checkpoint.state == harness::HarnessState::ManualReview);
    assert(recovered->checkpoint.outbox.back().state == harness::OutboxState::Unknown);
    assert(recovered->checkpoint.terminal_reason ==
           "terminal_conversation_with_recoverable_harness");

    const auto stale = reconciler.apply(first, false);
    assert(stale.conflicts == 1);
    assert(stale.changed == 0);

    auto tampered = first;
    tampered.findings.front().code = "tampered";
    const auto rejected = reconciler.apply(tampered, false);
    assert(rejected.errors.size() == 1);
    assert(rejected.errors.front() == "reconciliation_plan_digest_mismatch");

    // Historical-shape matrix: nine stale execution checkpoints, five
    // zero-iteration turns, plus durable attach/replay/manual-review evidence.
    const conversation::ConversationIdentity matrix_identity{"tenant", "matrix"};
    for(int i=0;i<5;++i) {
        conversation::TurnCheckpoint zero;
        zero.identity=matrix_identity;zero.turn_id="zero-"+std::to_string(i);
        zero.revision=1;zero.phase=conversation::TurnPhase::Running;
        assert(conversations.commit_turn(zero,0,nullptr));
    }
    for(int i=0;i<9;++i) {
        conversation::TurnCheckpoint terminal;
        terminal.identity=matrix_identity;terminal.turn_id="stale-"+std::to_string(i);
        terminal.revision=1;terminal.iteration=1;
        terminal.phase=conversation::TurnPhase::Failed;
        assert(conversations.commit_turn(terminal,0,nullptr));
        harness::HarnessCheckpoint stale_harness;
        stale_harness.metadata.identity.tenant_id="tenant";
        stale_harness.metadata.identity.task_id="task-"+std::to_string(i);
        stale_harness.metadata.extensions["conversation_id"]="matrix";
        stale_harness.harness_id="turn:"+terminal.turn_id;
        stale_harness.revision=1;stale_harness.state=harness::HarnessState::Running;
        stale_harness.next_stage=harness::HarnessStage::Execution;
        stale_harness.updated_at="1000";
        assert(harnesses.create(stale_harness,event_for(stale_harness,"created")));
    }
    tool_runtime::SQLiteInvocationStore invocations((root/"invocations.sqlite3").string());
    tool_runtime::LongRunningToolInvocation attachable;
    attachable.metadata.identity.tenant_id="tenant";
    attachable.metadata.identity.task_id="task-attach";
    attachable.metadata.identity.run_id="run-attach";
    attachable.invocation_id="inv-attach";attachable.conversation_id="matrix";
    attachable.turn_id="stale-0";attachable.tool_call_id="call-attach";
    attachable.tool_name="remote";attachable.tool_contract_revision="v1";
    attachable.deployment_revision="d1";attachable.tool_generation="g1";
    attachable.input_digest="sha256:input";attachable.created_at="1000";
    attachable.updated_at="1000";attachable.adapter_restart_policy="attach";
    attachable.external_operation_id="external-1";
    assert(invocations.create(attachable));
    ToolEffectJournal effects(root/"effects.jsonl");
    ToolEffectRecord replayable;replayable.task_id="task-replay";
    replayable.session_id="matrix";replayable.tool_name="remote";
    replayable.tool_call_id="call-replay";replayable.idempotency_key="effect-replay";
    replayable.request_digest="sha256:request-replay";replayable.safe_to_replay=true;
    replayable.reconciliation_policy=ToolReconciliationPolicy::ReplayIdempotent;
    assert(effects.begin(replayable)==ToolEffectBeginResult::Started);
    ToolEffectRecord unknown=replayable;unknown.tool_call_id="call-unknown";
    unknown.idempotency_key="effect-unknown";unknown.request_digest="sha256:request-unknown";
    unknown.safe_to_replay=false;unknown.reconciliation_policy=ToolReconciliationPolicy::ManualReview;
    assert(effects.begin(unknown)==ToolEffectBeginResult::Started);
    recovery::SystemStateReconciler matrix_reconciler(
        conversations,harnesses,&invocations,&effects);
    const auto matrix=matrix_reconciler.scan({matrix_identity,100});
    const auto count=[&](std::string_view code){return std::count_if(
        matrix.findings.begin(),matrix.findings.end(),[&](const auto& finding){
            return finding.code==code;});};
    assert(count("conversation_zero_iteration_nonterminal")==5);
    assert(count("terminal_conversation_with_recoverable_harness")==9);
    assert(count("attachable_invocation_recoverable")==1);
    assert(count("idempotent_effect_replayable")==1);
    assert(count("unknown_non_idempotent_effect")==1);
    const auto matrix_dry=matrix_reconciler.apply(matrix,true);
    assert(matrix_dry.inspected==17&&matrix_dry.changed==0);
    const auto matrix_applied=matrix_reconciler.apply(matrix,false);
    assert(matrix_applied.changed==10&&matrix_applied.errors.empty());
    assert(effects.find_idempotency("effect-unknown")->status==ToolEffectStatus::ManualReview);
    assert(effects.find_idempotency("effect-replay")->status==ToolEffectStatus::Started);

    std::filesystem::remove_all(root, ec);
    return 0;
}
