#include <cassert>
#include <filesystem>
#include <string>

#include "agent/internal/platform_io.hpp"
#include "agent/run/state_machine.hpp"
#include "agent/run/store.hpp"

namespace {
using namespace agent_framework;

run::RunCheckpoint checkpoint(run::RunState state) {
    run::RunCheckpoint value;
    value.metadata.identity.tenant_id = "tenant-a";
    value.metadata.identity.task_id = "task-a";
    value.metadata.identity.run_id = "run-a";
    value.state = state;
    value.graph_revision = "graph-v1";
    value.plan_digest = "sha256:plan";
    value.memory_snapshot_id = "snapshot-a";
    value.memory_view_digest = "sha256:view";
    return value;
}
}

int main() {
    using namespace agent_framework;
    assert(run::can_transition(run::RunState::Created, run::RunState::Received));
    assert(!run::can_transition(run::RunState::Created, run::RunState::Completed));
    assert(!run::can_transition(run::RunState::Completed, run::RunState::Running));

    const auto root = std::filesystem::temp_directory_path() /
        ("agent-phase4-run-" + std::to_string(internal::current_process_id()));
    const auto path = root / "run.sqlite3";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    {
        run::SQLiteRunStore store(path.string());
        auto initial = checkpoint(run::RunState::Created);
        assert(store.create(initial).status == run::StoreStatus::Committed);
        assert(store.create(initial).status == run::StoreStatus::AlreadyExists);
        auto loaded = store.load("run-a");
        assert(loaded && loaded->revision == 1 && loaded->checkpoint.state == run::RunState::Created);

        auto received = checkpoint(run::RunState::Received);
        assert(store.checkpoint(received, 1).revision == 2);
        assert(store.checkpoint(received, 1).status == run::StoreStatus::RevisionConflict);
        auto invalid = checkpoint(run::RunState::Completed);
        assert(store.checkpoint(invalid, 2).status == run::StoreStatus::Invalid);

        assert(store.append_event({"run-a", 0, "run_received", {{"ok", true}}}).revision == 1);
        assert(store.append_event({"run-a", 0, "planning_started", {{"step", 1}}}).revision == 2);
        auto events = store.events("run-a");
        assert(events.size() == 2 && events[0].sequence == 1 && events[1].sequence == 2);

        assert(store.register_graph({"planning", "v1", "sha256:graph", "phase4-v1"}));
        assert(store.register_graph({"planning", "v1", "sha256:graph", "phase4-v1"}));
        assert(store.register_graph({"planning", "v1", "sha256:changed", "phase4-v1"}).status ==
               run::StoreStatus::RevisionConflict);

        run::Interruption interruption;
        interruption.metadata = initial.metadata;
        interruption.interruption_id = "interrupt-a";
        interruption.kind = "plan_approval";
        interruption.resume_token_digest = "sha256:token";
        interruption.state_digest = "sha256:state";
        assert(store.put_interruption(interruption));
        assert(store.load_interruption("interrupt-a"));
        assert(store.consume_resume_token("interrupt-a", "other-run", "sha256:token").status ==
               run::StoreStatus::Invalid);
        assert(store.consume_resume_token("interrupt-a", "run-a", "sha256:token"));
        assert(store.consume_resume_token("interrupt-a", "run-a", "sha256:token").status ==
               run::StoreStatus::Invalid);

        assert(store.schedule_timer({"timer-a", "run-a", 1000, {{"attempt", 2}}}));
        assert(store.claim_due_timers(999, "worker-a", 100, 10).empty());
        auto claimed = store.claim_due_timers(1000, "worker-a", 100, 10);
        assert(claimed.size() == 1 && claimed.front().owner == "worker-a");
        assert(store.claim_due_timers(1001, "worker-b", 100, 10).empty());
        assert(store.complete_timer("timer-a", "worker-b").status == run::StoreStatus::Invalid);
        assert(store.complete_timer("timer-a", "worker-a"));

        auto planning = checkpoint(run::RunState::Planning);
        assert(store.checkpoint(planning, 2).revision == 3);
        assert(store.list_recoverable(10).size() == 1);
    }

    {
        run::SQLiteRunStore reopened(path.string());
        auto loaded = reopened.load("run-a");
        assert(loaded && loaded->revision == 3 && loaded->checkpoint.state == run::RunState::Planning);
        auto running = checkpoint(run::RunState::Running);
        assert(reopened.checkpoint(running, 3).revision == 4);
        auto verifying = checkpoint(run::RunState::Verifying);
        assert(reopened.checkpoint(verifying, 4).revision == 5);
        auto complete = checkpoint(run::RunState::Completed);
        assert(reopened.checkpoint(complete, 5).revision == 6);
        assert(reopened.list_recoverable(10).empty());
    }

#if !defined(_WIN32)
    const auto permissions = std::filesystem::status(path).permissions();
    assert((permissions & std::filesystem::perms::group_all) == std::filesystem::perms::none);
    assert((permissions & std::filesystem::perms::others_all) == std::filesystem::perms::none);
#endif
    std::filesystem::remove_all(root, error);
    return 0;
}
