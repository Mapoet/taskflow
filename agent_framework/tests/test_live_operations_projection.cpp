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
    fs::remove_all(root, ec);
    return 0;
}
