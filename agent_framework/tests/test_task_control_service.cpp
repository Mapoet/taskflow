#include <cassert>
#include <filesystem>
#include "agent/conversation/task_control_service.hpp"
#include "agent/internal/platform_io.hpp"
int main()
{
    using namespace agent_framework;
    namespace fs = std::filesystem;
    auto root = fs::temp_directory_path() / (std::string("task-control-") + std::to_string(internal::current_process_id()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    auto db = (root / "state.sqlite3").string();
    conversation::SQLiteTaskRegistry tasks(db);
    tool_runtime::SQLiteInvocationStore invocations(db);
    tool_runtime::SQLiteExecutionControlStore controls(db);
    conversation::ConversationIdentity identity{"tenant", "conversation"};
    conversation::PersistentTask task;
    task.identity = identity;
    task.task_id = "task";
    task.root_turn_id = "turn";
    task.current_turn_id = "turn";
    task.current_run_id = "run";
    assert(tasks.create(task, {identity, "task", 1, conversation::TaskInputIntent::InitialRequest, "turn", "work"}, {identity, "turn", "task", "run", 1, conversation::TaskInputIntent::InitialRequest}).ok);
    auto loaded = tasks.load(identity, "task");
    assert(loaded);
    tool_runtime::LongRunningToolInvocation v;
    v.metadata.identity.tenant_id = "tenant";
    v.metadata.identity.task_id = "task";
    v.metadata.identity.run_id = "run";
    v.invocation_id = "inv";
    v.conversation_id = "conversation";
    v.turn_id = "turn";
    v.tool_call_id = "call";
    v.tool_name = "tool";
    v.tool_contract_revision = "v1";
    v.deployment_revision = "d";
    v.tool_generation = "g";
    v.input_digest = "sha256:i";
    v.created_at = v.updated_at = "now";
    assert(invocations.create(v));
    assert(controls.create({"inv", "tenant", 0, 0, {}}));
    conversation::TaskControlService service(tasks, invocations, controls);
    auto status = service.status(identity, "task");
    assert(status.at("found") && status.at("runs").size() == 1 && status.at("runs")[0].at("invocations").size() == 1);
    auto cancelled = service.cancel(identity, "task", "user", 100);
    assert(cancelled.ok && cancelled.affected == 1);
    assert(controls.load("inv")->stage == tool_runtime::CancellationStage::Requested);
    fs::remove_all(root, ec);
}
