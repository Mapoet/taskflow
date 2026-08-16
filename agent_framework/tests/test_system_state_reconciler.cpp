#include <agent/conversation/store.hpp>
#include <agent/harness/store.hpp>
#include <agent/recovery/system_state_reconciler.hpp>
#include <agent/internal/platform_io.hpp>

#include <cassert>
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
    assert(dry.manual_review == 1);
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

    std::filesystem::remove_all(root, ec);
    return 0;
}
