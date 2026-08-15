#include <cassert>
#include <filesystem>

#include "agent/conversation/task_registry.hpp"
#include "agent/internal/platform_io.hpp"

int main() {
    using namespace agent_framework::conversation;
    namespace fs = std::filesystem;
    const auto path = fs::temp_directory_path() /
        ("conversation-task-registry-" +
         std::to_string(agent_framework::internal::current_process_id()) + ".sqlite3");
    std::error_code error;
    fs::remove(path, error);
    const ConversationIdentity identity{"tenant", "session"};

    {
        SQLiteTaskRegistry registry(path.string());
        PersistentTask task;
        task.identity = identity;
        task.task_id = "task-1";
        task.root_turn_id = "turn-1";
        task.current_turn_id = "turn-1";
        task.current_run_id = "run-1";
        TaskRequirementRevision requirement;
        requirement.identity = identity;
        requirement.task_id = task.task_id;
        requirement.turn_id = "turn-1";
        requirement.content = "produce a report";
        TurnTaskLink link;
        link.identity = identity;
        link.turn_id = "turn-1";
        link.intent = TaskInputIntent::InitialRequest;
        auto created = registry.create(task, requirement, link);
        assert(created.ok && created.revision == 1);
        assert(!registry.create(task, requirement, link).ok);
        auto active = registry.active(identity);
        assert(active && active->task_id == "task-1");
        assert(active->current_run_id == "run-1");
        auto first_link = registry.link_for_turn(identity, "turn-1");
        assert(first_link && first_link->task_id == "task-1");

        TaskRequirementRevision continuation;
        continuation.identity = identity;
        continuation.task_id = "task-1";
        continuation.turn_id = "turn-2";
        continuation.intent = TaskInputIntent::ContinueTask;
        continuation.content = "continue";
        TurnTaskLink continuation_link;
        continuation_link.identity = identity;
        continuation_link.turn_id = "turn-2";
        continuation_link.task_id = "task-1";
        continuation_link.intent = TaskInputIntent::ContinueTask;
        auto revised = registry.append_requirement(
            continuation, continuation_link, active->revision, "run-2");
        assert(revised.ok && revised.revision == 2);
        auto stale = registry.append_requirement(
            continuation, continuation_link, active->revision, "run-stale");
        assert(!stale.ok);
        auto requirements = registry.requirements(identity, "task-1");
        assert(requirements.size() == 2);
        assert(requirements[1].previous_digest == requirements[0].digest);
        assert(requirements[1].intent == TaskInputIntent::ContinueTask);
        active = registry.active(identity);
        assert(active && active->revision == 2 &&
               active->requirement_revision == 2 &&
               active->current_turn_id == "turn-2" &&
               active->current_run_id == "run-2");
        auto linked_runs = registry.runs(identity, "task-1");
        assert(linked_runs.size() == 1 && linked_runs.front().run_id == "run-1");
        TurnTaskLink status{identity, "turn-status", "task-1", "run-2", 0,
                            TaskInputIntent::StatusQuery};
        auto attached = registry.attach_turn(status, active->revision);
        assert(attached.ok && attached.revision == 3);
        assert(registry.requirements(identity, "task-1").size() == 2);
        active = registry.active(identity);
        assert(active && active->current_turn_id == "turn-status");
        TaskRunLink second_run{identity, "task-1", "run-2b", 0, 7, "active"};
        auto bound = registry.bind_run(second_run, active->revision);
        assert(bound.ok && bound.revision == 4);
        linked_runs = registry.runs(identity, "task-1");
        assert(linked_runs.size() == 2 && linked_runs.back().plan_revision == 7);
    }

    {
        SQLiteTaskRegistry reopened(path.string());
        auto active = reopened.active(identity);
        assert(active && active->task_id == "task-1" && active->revision == 4);
        auto cancelled = reopened.transition(identity, "task-1", active->revision,
            TaskLifecycleState::Cancelled, "cancelled");
        assert(cancelled.ok && cancelled.revision == 5);
        assert(!reopened.active(identity));
        auto stored = reopened.load(identity, "task-1");
        assert(stored && stored->state == TaskLifecycleState::Cancelled &&
               stored->closure_state == "cancelled");
    }

    assert(classify_task_input("继续", true) == TaskInputIntent::ContinueTask);
    assert(classify_task_input("现在怎么样", true) == TaskInputIntent::StatusQuery);
    assert(classify_task_input("换方案", true) == TaskInputIntent::ReplanTask);
    assert(classify_task_input("先停一下", true) == TaskInputIntent::SuspendTask);
    assert(classify_task_input("取消任务", true) == TaskInputIntent::CancelTask);
    assert(classify_task_input("生成一张图", true) == TaskInputIntent::AmendRequirements);
    assert(classify_task_input("/new another task", true) ==
           TaskInputIntent::StartNewTask);

    fs::remove(path, error);
    return 0;
}
