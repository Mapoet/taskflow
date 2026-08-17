#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <filesystem>
#include <thread>

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
        auto create_replay = registry.create(task, requirement, link);
        assert(create_replay.ok && create_replay.revision == 1);
        auto conflicting_requirement = requirement;
        conflicting_requirement.content = "different initial request";
        const auto create_conflict = registry.create(
            task, conflicting_requirement, link);
        assert(!create_conflict.ok && create_conflict.error ==
               "task_create_idempotency_conflict");
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
        auto replayed = registry.append_requirement(
            continuation, continuation_link, active->revision, "run-2");
        assert(replayed.ok && replayed.revision == 2);
        auto conflicting_continuation = continuation;
        conflicting_continuation.content = "different replay";
        auto replay_conflict = registry.append_requirement(
            conflicting_continuation, continuation_link, active->revision,
            "run-2");
        assert(!replay_conflict.ok && replay_conflict.error ==
               "task_turn_idempotency_conflict");
        TaskRequirementRevision reused_run = continuation;
        reused_run.turn_id = "turn-reused-run";
        reused_run.content = "new input with an already bound run id";
        TurnTaskLink reused_run_link = continuation_link;
        reused_run_link.turn_id = reused_run.turn_id;
        const auto reused_run_result = registry.append_requirement(
            reused_run, reused_run_link, revised.revision, "run-2");
        assert(!reused_run_result.ok && reused_run_result.error ==
               "task_run_link_conflict");
        assert(!registry.link_for_turn(identity, reused_run.turn_id));
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
        assert(linked_runs.size() == 2 && linked_runs[0].run_id == "run-1" &&
               linked_runs[1].run_id == "run-2" &&
               linked_runs[1].state == "awaiting_plan");
        TaskRunLink first_plan{identity, "task-1", "run-2", 2, 1, "planned"};
        first_plan.plan_digest = "sha256:plan-1";
        first_plan.task_contract_digest = "sha256:task-contract-2";
        auto planned = registry.bind_plan(first_plan, active->revision);
        assert(planned.ok && planned.revision == 3);
        active = registry.active(identity);
        assert(active && active->plan_revision == 1);
        linked_runs = registry.runs(identity, "task-1");
        assert(linked_runs[1].plan_revision == 1 &&
               linked_runs[1].plan_digest == "sha256:plan-1");
        auto plan_replay = registry.bind_plan(first_plan, revised.revision);
        assert(plan_replay.ok && plan_replay.revision == active->revision);
        auto conflicting_plan = first_plan;
        conflicting_plan.plan_digest = "sha256:different-plan";
        const auto plan_conflict = registry.bind_plan(
            conflicting_plan, active->revision);
        assert(!plan_conflict.ok && plan_conflict.error ==
               "task_run_plan_digest_conflict");
        TurnTaskLink status{identity, "turn-status", "task-1", "run-2", 0,
                            TaskInputIntent::StatusQuery};
        auto attached = registry.attach_turn(status, active->revision);
        assert(attached.ok && attached.revision == 4);
        assert(registry.requirements(identity, "task-1").size() == 2);
        active = registry.active(identity);
        assert(active && active->current_turn_id == "turn-status");
        TaskRunLink second_run{identity, "task-1", "run-2b", 0, 7, "active"};
        auto bound = registry.bind_run(second_run, active->revision);
        assert(bound.ok && bound.revision == 5);
        auto run_replay = registry.bind_run(second_run, active->revision);
        assert(run_replay.ok && run_replay.revision == 5);
        linked_runs = registry.runs(identity, "task-1");
        assert(linked_runs.size() == 3 && linked_runs.back().run_id == "run-2b" &&
               linked_runs.back().plan_revision == 7);
    }

    {
        SQLiteTaskRegistry reopened(path.string());
        auto active = reopened.active(identity);
        assert(active && active->task_id == "task-1" && active->revision == 5);
        auto cancelled = reopened.transition(identity, "task-1", active->revision,
            TaskLifecycleState::Cancelled, "cancelled");
        assert(cancelled.ok && cancelled.revision == 6);
        assert(!reopened.active(identity));
        auto stored = reopened.load(identity, "task-1");
        assert(stored && stored->state == TaskLifecycleState::Cancelled &&
               stored->closure_state == "cancelled");
    }

    const auto concurrent_path = fs::temp_directory_path() /
        ("conversation-task-registry-concurrent-" +
         std::to_string(agent_framework::internal::current_process_id()) +
         ".sqlite3");
    fs::remove(concurrent_path, error);
    {
        SQLiteTaskRegistry concurrent(concurrent_path.string());
        PersistentTask task;
        task.identity = identity;
        task.task_id = "task-concurrent";
        task.root_turn_id = "turn-root";
        task.current_turn_id = "turn-root";
        task.current_run_id = "run-root";
        TaskRequirementRevision initial;
        initial.identity = identity;
        initial.task_id = task.task_id;
        initial.turn_id = task.root_turn_id;
        initial.content = "root";
        TurnTaskLink root_link{identity, task.root_turn_id, task.task_id,
                               task.current_run_id, 1,
                               TaskInputIntent::InitialRequest};
        assert(concurrent.create(task, initial, root_link).ok);
        auto make_requirement = [&](std::string turn) {
            TaskRequirementRevision value;
            value.identity = identity;
            value.task_id = task.task_id;
            value.turn_id = std::move(turn);
            value.intent = TaskInputIntent::AmendRequirements;
            value.content = value.turn_id;
            return value;
        };
        auto first = make_requirement("turn-a");
        auto second = make_requirement("turn-b");
        TurnTaskLink first_link{identity, first.turn_id, task.task_id, "run-a", 0,
                                first.intent};
        TurnTaskLink second_link{identity, second.turn_id, task.task_id, "run-b", 0,
                                 second.intent};
        TaskMutationResult first_result, second_result;
        std::thread one([&] {
            first_result = concurrent.append_requirement(
                first, first_link, 1, "run-a");
        });
        std::thread two([&] {
            second_result = concurrent.append_requirement(
                second, second_link, 1, "run-b");
        });
        one.join();
        two.join();
        assert(first_result.ok != second_result.ok);
        const auto& loser = first_result.ok ? second_result : first_result;
        assert(loser.error == "task_revision_conflict");
        assert(concurrent.requirements(identity, task.task_id).size() == 2);
        assert(concurrent.runs(identity, task.task_id).size() == 2);
    }
    fs::remove(concurrent_path, error);

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
