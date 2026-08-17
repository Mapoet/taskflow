#include "agent/conversation/task_orchestrator.hpp"

namespace agent_framework::conversation {
namespace {
bool is_control(TaskInputIntent intent) {
    return intent == TaskInputIntent::StatusQuery ||
           intent == TaskInputIntent::CancelTask ||
           intent == TaskInputIntent::SuspendTask;
}
}

std::string select_task_run_id(std::string_view configured_run_id,
                               std::string_view active_run_id,
                               bool control_only,
                               std::string_view turn_id) {
    if(!configured_run_id.empty()) return std::string(configured_run_id);
    if(control_only && !active_run_id.empty()) return std::string(active_run_id);
    return std::string(turn_id);
}

TaskOpenResult TaskOrchestrator::open_or_resume(
    const TurnRequest& request, TaskInputIntent intent) {
    TaskOpenResult result;
    result.intent = intent;
    result.control_only = is_control(intent);
    if(request.identity.tenant_id.empty() || request.identity.conversation_id.empty() ||
       request.turn_id.empty() || request.task_id.empty() || request.run_id.empty()) {
        result.error = "task_open_contract_invalid";
        return result;
    }
    auto active = registry_.active(request.identity);
    if(active && active->task_id != request.task_id)
        active = registry_.load(request.identity, request.task_id);
    const bool force_new = intent == TaskInputIntent::StartNewTask;
    if(force_new) {
        auto current = registry_.active(request.identity);
        if(current) {
            const auto suspended = registry_.transition(
                request.identity, current->task_id, current->revision,
                TaskLifecycleState::Suspended, "suspended_by_new_active_task");
            if(!suspended.ok) {
                result.error = "task_registry_suspend_failed:" + suspended.error;
                return result;
            }
        }
        active.reset();
    }
    if(result.control_only && !active) {
        result.error = "task_context_required_for_control_request";
        return result;
    }
    if(active) {
        auto expected = active->revision;
        if(active->state == TaskLifecycleState::Suspended &&
           (intent == TaskInputIntent::ContinueTask || intent == TaskInputIntent::ReplanTask)) {
            const auto resumed = registry_.transition(
                request.identity, request.task_id, expected,
                TaskLifecycleState::Active, "running");
            if(!resumed.ok) {
                result.error = "task_registry_resume_failed:" + resumed.error;
                return result;
            }
            expected = resumed.revision;
        }
        TurnTaskLink link{request.identity, request.turn_id, request.task_id,
                          request.run_id, 0, intent};
        TaskMutationResult mutation;
        if(result.control_only) {
            mutation = registry_.attach_turn(link, expected);
            if(mutation.ok && (intent == TaskInputIntent::CancelTask ||
                               intent == TaskInputIntent::SuspendTask)) {
                const auto state = intent == TaskInputIntent::CancelTask
                    ? TaskLifecycleState::Cancelled : TaskLifecycleState::Suspended;
                mutation = registry_.transition(
                    request.identity, request.task_id, mutation.revision, state,
                    intent == TaskInputIntent::CancelTask
                        ? "cancel_requested" : "suspended_by_user");
            }
        } else {
            TaskRequirementRevision requirement{
                request.identity, request.task_id, 0, intent,
                request.turn_id, request.input};
            mutation = registry_.append_requirement(
                requirement, link, expected, request.run_id);
        }
        if(!mutation.ok) {
            result.error = "task_registry_mutation_failed:" + mutation.error;
            return result;
        }
    } else {
        PersistentTask task;
        task.identity = request.identity;
        task.task_id = request.task_id;
        task.root_turn_id = request.turn_id;
        task.current_turn_id = request.turn_id;
        task.current_run_id = request.run_id;
        TaskRequirementRevision requirement{
            request.identity, request.task_id, 1, intent,
            request.turn_id, request.input};
        TurnTaskLink link{request.identity, request.turn_id, request.task_id,
                          request.run_id, 1, intent};
        const auto mutation = registry_.create(task, requirement, link);
        if(!mutation.ok) {
            result.error = "task_registry_create_failed:" + mutation.error;
            return result;
        }
        result.created = true;
    }
    auto stored = registry_.load(request.identity, request.task_id);
    if(!stored) {
        result.error = "task_registry_postcondition_missing";
        return result;
    }
    result.task = std::move(*stored);
    result.ok = true;
    return result;
}

TaskMutationResult TaskOrchestrator::bind_plan(
    const TaskPlanBinding& binding, std::uint64_t expected) {
    TaskRunLink link;
    link.identity = binding.identity;
    link.task_id = binding.task_id;
    link.run_id = binding.run_id;
    link.requirement_revision = binding.requirement_revision;
    link.plan_revision = binding.plan_revision;
    link.state = "planned";
    link.plan_digest = binding.plan_digest;
    link.task_contract_digest = binding.task_contract_digest;
    return registry_.bind_plan(link, expected);
}

}  // namespace agent_framework::conversation
