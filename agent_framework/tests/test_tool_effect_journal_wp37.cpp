#include <agent/toolbus/tool_effect_journal.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>

using namespace agent_framework;

ToolEffectRecord record(std::string key, std::string digest,
                        ToolReconciliationPolicy policy, bool replay) {
    ToolEffectRecord value;
    value.task_id = "task";
    value.session_id = "session";
    value.tool_name = "write_remote";
    value.tool_call_id = "call";
    value.idempotency_key = std::move(key);
    value.request_digest = std::move(digest);
    value.attempt = 1;
    value.reconciliation_policy = policy;
    value.safe_to_replay = replay;
    return value;
}

int main() {
    const auto root = std::filesystem::temp_directory_path() / "agent-effect-wp37";
    const auto wal = root / "effects.jsonl";
    std::filesystem::remove_all(root);

    {
        ToolEffectJournal journal(wal);
        assert(journal.begin(record("read-key", "request-a",
                                    ToolReconciliationPolicy::ReplayIdempotent, true)) ==
               ToolEffectBeginResult::Started);
        assert(journal.begin(record("read-key", "request-a",
                                    ToolReconciliationPolicy::ReplayIdempotent, true)) ==
               ToolEffectBeginResult::ExistingInFlight);
        assert(journal.begin(record("read-key", "different-request",
                                    ToolReconciliationPolicy::ReplayIdempotent, true)) ==
               ToolEffectBeginResult::Conflict);
    }
    // Fault window 1: restart after durable started, before tool invocation completed.
    {
        ToolEffectJournal recovered(wal);
        const auto pending = recovered.find_idempotency("read-key");
        assert(pending && pending->status == ToolEffectStatus::Started);
        assert(recovered.reconciliation_action(*pending) == ToolReconciliationAction::Replay);
        assert(recovered.complete("read-key", "result-a"));
    }
    // Fault window 2/3: completed before session commit must reconcile by lookup, never replay blindly.
    {
        ToolEffectJournal recovered(wal);
        const auto completed = recovered.find_idempotency("read-key");
        assert(completed && completed->status == ToolEffectStatus::Completed);
        assert(recovered.reconciliation_action(*completed) == ToolReconciliationAction::Lookup);
        assert(recovered.commit("read-key"));
    }
    // Fault window 4: session+journal committed has no recovery action and rejects duplicate execution.
    {
        ToolEffectJournal recovered(wal);
        const auto committed = recovered.find_idempotency("read-key");
        assert(committed && !committed->committed_at.empty());
        assert(recovered.reconciliation_action(*committed) == ToolReconciliationAction::None);
        assert(recovered.begin(record("read-key", "request-a",
                                    ToolReconciliationPolicy::ReplayIdempotent, true)) ==
               ToolEffectBeginResult::ExistingCommitted);

        assert(recovered.begin(record("unsafe-key", "request-b",
                                    ToolReconciliationPolicy::ManualReview, false)) ==
               ToolEffectBeginResult::Started);
        assert(recovered.mark_manual_review("unsafe-key", "external_outcome_unknown"));
        const auto unsafe = recovered.find_idempotency("unsafe-key");
        assert(unsafe && recovered.reconciliation_action(*unsafe) ==
                         ToolReconciliationAction::ManualReview);

        assert(recovered.begin(record("cancel-key", "request-c",
                                    ToolReconciliationPolicy::FailClosed, false)) ==
               ToolEffectBeginResult::Started);
        assert(recovered.cancel("cancel-key"));
        assert(!recovered.commit("cancel-key"));
    }
    std::ifstream stored(wal);
    const std::string bytes((std::istreambuf_iterator<char>(stored)), {});
    assert(bytes.find("started_at") != std::string::npos);
    assert(bytes.find("committed_at") != std::string::npos);
    std::filesystem::remove_all(root);
}
