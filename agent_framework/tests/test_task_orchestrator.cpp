#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <filesystem>

#include "agent/conversation/task_orchestrator.hpp"
#include "agent/internal/platform_io.hpp"

int main() {
    using namespace agent_framework::conversation;
    assert(explicit_task_control_input("/cancel")==TaskInputIntent::CancelTask);
    assert(explicit_task_control_input("状态")==TaskInputIntent::StatusQuery);
    assert(explicit_task_control_input("/stop")==TaskInputIntent::SuspendTask);
    assert(!explicit_task_control_input("请分析取消机制"));
    const auto path = std::filesystem::temp_directory_path() /
        ("task-orchestrator-" + std::to_string(
            agent_framework::internal::current_process_id()) + ".sqlite3");
    std::error_code error;
    std::filesystem::remove(path, error);
    SQLiteTaskRegistry registry(path.string());
    TaskOrchestrator orchestrator(registry);
    TurnRequest initial{{"tenant", "conversation"}, "turn-1", "build it",
                        TaskExecutionProfile::CodeChange, 10};
    initial.task_id = "task-1";
    initial.run_id = "run-1";
    auto opened = orchestrator.open_or_resume(initial, TaskInputIntent::InitialRequest);
    assert(opened.ok && opened.created && opened.task.requirement_revision == 1);

    TaskPlanBinding binding{initial.identity, "task-1", "run-1", 1, 1,
                            "sha256:plan", "sha256:contract"};
    auto planned = orchestrator.bind_plan(binding, opened.task.revision);
    assert(planned.ok);
    auto task = registry.load(initial.identity, "task-1");
    assert(task && task->plan_revision == 1);
    assert(orchestrator.bind_plan(binding, task->revision).ok);

    assert(select_task_run_id("", "run-active", false, "turn-next") ==
           "turn-next");
    assert(select_task_run_id("", "run-active", true, "turn-status") ==
           "run-active");
    assert(select_task_run_id("run-configured", "run-active", false,
                              "turn-next") == "run-configured");

    TurnRequest continuation = initial;
    continuation.turn_id = "turn-2";
    continuation.input = "continue";
    continuation.run_id = select_task_run_id(
        "", task->current_run_id, false, continuation.turn_id);
    assert(continuation.run_id == "turn-2");
    auto continued = orchestrator.open_or_resume(
        continuation, TaskInputIntent::ContinueTask);
    assert(continued.ok && continued.task.requirement_revision == 2);
    const auto replayed = orchestrator.open_or_resume(
        continuation, TaskInputIntent::ContinueTask);
    assert(replayed.ok && replayed.task.revision == continued.task.revision);
    assert(registry.requirements(initial.identity, "task-1").size() == 2);

    TurnRequest status = continuation;
    status.turn_id = "turn-status";
    status.input = "/status";
    auto observed = orchestrator.open_or_resume(status, TaskInputIntent::StatusQuery);
    assert(observed.ok && observed.control_only);
    assert(registry.requirements(initial.identity, "task-1").size() == 2);

    TurnRequest second = initial;
    second.turn_id = "turn-new";
    second.task_id = "task-2";
    second.run_id = "run-new";
    second.input = "/new another";
    auto created = orchestrator.open_or_resume(second, TaskInputIntent::StartNewTask);
    assert(created.ok && created.created && created.task.task_id == "task-2");
    auto old = registry.load(initial.identity, "task-1");
    assert(old && old->state == TaskLifecycleState::Suspended);
    // The active pointer is deterministic after explicit task switching: the
    // previous task cannot remain an ambiguous active candidate.
    auto active = registry.active(initial.identity);
    assert(active && active->task_id == "task-2");
    std::filesystem::remove(path, error);
}
