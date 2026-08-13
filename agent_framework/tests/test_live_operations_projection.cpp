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
    const auto serialized = Phase4OperationsProjection::to_json(done).dump();
    assert(serialized.find("must-not-be-projected") == std::string::npos);

    auto replay = store->latest("tenant-live", "run-live");
    assert(replay && replay->snapshot_id == done.snapshot_id);
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
    fs::remove_all(root, ec);
    return 0;
}
