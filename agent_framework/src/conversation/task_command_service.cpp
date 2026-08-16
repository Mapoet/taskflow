#include "agent/conversation/task_command_service.hpp"

#include <algorithm>

namespace agent_framework::conversation {
namespace {
std::string required_scope(TaskCommandKind command) {
    if(command==TaskCommandKind::Status||command==TaskCommandKind::Output)
        return "task:read";
    if(command==TaskCommandKind::Suspend||command==TaskCommandKind::Cancel)
        return "task:control";
    return "task:write";
}
bool has_scope(const TaskCommandPrincipal& principal,std::string_view scope) {
    return std::find(principal.scopes.begin(),principal.scopes.end(),scope)!=
           principal.scopes.end();
}
bool terminal(TaskLifecycleState state) {
    return state==TaskLifecycleState::Closed||state==TaskLifecycleState::Failed||
           state==TaskLifecycleState::Cancelled;
}
bool state_allows(TaskCommandKind command,const std::optional<PersistentTask>& task) {
    if(command==TaskCommandKind::Start)return true;
    if(!task)return false;
    if(command==TaskCommandKind::Status||command==TaskCommandKind::Output)return true;
    if(terminal(task->state))return false;
    if(task->state==TaskLifecycleState::Closing)
        return command==TaskCommandKind::Cancel;
    if(command==TaskCommandKind::Continue)
        return task->state==TaskLifecycleState::Active||
               task->state==TaskLifecycleState::AwaitingInput||
               task->state==TaskLifecycleState::AwaitingApproval||
               task->state==TaskLifecycleState::Suspended;
    return true;
}
}

std::string TaskCommandPolicy::authorize(const TaskCommandPrincipal& principal,
    const TurnRequest& turn,TaskCommandKind command) const {
    if(!principal.authenticated||principal.actor_id.empty())
        return "task_command_authentication_required";
    if(principal.identity.tenant_id!=turn.identity.tenant_id)
        return "task_command_cross_tenant_forbidden";
    if(principal.identity.conversation_id!=turn.identity.conversation_id)
        return "task_command_cross_conversation_forbidden";
    if(!has_scope(principal,required_scope(command)))
        return "task_command_scope_forbidden:"+required_scope(command);
    return {};
}

std::vector<TaskCommandAction> TaskCommandPolicy::actions(
    const TaskCommandPrincipal& principal,const TurnRequest& turn,
    const std::optional<PersistentTask>& task) const {
    std::vector<TaskCommandAction> result;
    for(const auto command:{TaskCommandKind::Start,TaskCommandKind::Status,
        TaskCommandKind::Output,TaskCommandKind::Continue,TaskCommandKind::Amend,
        TaskCommandKind::Suspend,TaskCommandKind::Cancel,TaskCommandKind::Attach,
        TaskCommandKind::Replan}) {
        TaskCommandAction action;action.command=command;
        action.required_scope=required_scope(command);
        action.reason=authorize(principal,turn,command);
        if(action.reason.empty()&&!state_allows(command,task))
            action.reason=task?"task_command_not_allowed_in_state:"+
                std::string(name(task->state)):"task_command_task_required";
        action.enabled=action.reason.empty();result.push_back(std::move(action));
    }
    return result;
}

std::string_view name(TaskCommandKind command) noexcept {
    switch(command){
        case TaskCommandKind::Start:return "start";case TaskCommandKind::Status:return "status";
        case TaskCommandKind::Output:return "output";case TaskCommandKind::Continue:return "continue";
        case TaskCommandKind::Amend:return "amend";case TaskCommandKind::Suspend:return "suspend";
        case TaskCommandKind::Cancel:return "cancel";case TaskCommandKind::Attach:return "attach";
        case TaskCommandKind::Replan:return "replan";
    }return "status";
}

TaskCommandResponse TaskCommandService::execute(const TaskCommandRequest& request) {
    TaskCommandResponse response;
    const auto& turn=request.turn;
    if(turn.identity.tenant_id.empty()||turn.identity.conversation_id.empty()||turn.task_id.empty()){
        response.error="task_command_identity_required";return response;
    }
    if(policy_){
        if(!request.principal){response.error="task_command_authentication_required";return response;}
        response.error=policy_->authorize(*request.principal,turn,request.command);
        if(!response.error.empty())return response;
    }
    auto current=tasks_.load(turn.identity,turn.task_id);
    const auto attach_actions=[&](nlohmann::json& payload,
                                  const std::optional<PersistentTask>& task) {
        if(!policy_||!request.principal)return;
        nlohmann::json actions=nlohmann::json::array();
        for(const auto& action:policy_->actions(*request.principal,turn,task))
            actions.push_back({{"command",name(action.command)},
                {"required_scope",action.required_scope},{"enabled",action.enabled},
                {"reason",action.reason}});
        payload["actions"]=std::move(actions);
    };
    if(request.expected_task_revision&&
       (!current||current->revision!=*request.expected_task_revision)){
        response.error="task_command_stale_revision";return response;
    }
    if(!state_allows(request.command,current)){
        response.error=current?"task_command_not_allowed_in_state:"+
            std::string(name(current->state)):"task_command_task_required";
        return response;
    }
    if(request.command==TaskCommandKind::Status||request.command==TaskCommandKind::Output){
        response.read_only=true;
        auto task=tasks_.load(turn.identity,turn.task_id);
        if(!task){response.error="task_not_found";return response;}
        response.task_revision=task->revision;
        response.payload=controls_?controls_->status(turn.identity,turn.task_id)
            :nlohmann::json{{"found",true},{"task",encode(*task)}};
        attach_actions(response.payload,task);
        if(request.command==TaskCommandKind::Output&&response.payload.contains("runs")){
            nlohmann::json output=nlohmann::json::array();
            for(const auto& run:response.payload["runs"])
                for(const auto& invocation:run.value("invocations",nlohmann::json::array()))
                    if(invocation.contains("partial_output"))output.push_back(invocation["partial_output"]);
            response.payload={{"task_id",turn.task_id},{"outputs",std::move(output)},
                              {"task_revision",task->revision}};
            attach_actions(response.payload,task);
        }
        response.ok=true;return response;
    }
    if(turn.turn_id.empty()||turn.run_id.empty()){
        response.error="task_command_turn_and_run_required";return response;
    }
    if(request.command==TaskCommandKind::Attach){
        auto task=tasks_.load(turn.identity,turn.task_id);
        if(!task){response.error="task_not_found";return response;}
        TurnTaskLink link{turn.identity,turn.turn_id,turn.task_id,turn.run_id,
                          task->requirement_revision,TaskInputIntent::ContinueTask};
        const auto attached=tasks_.attach_turn(link,task->revision);
        response.ok=attached.ok;response.task_revision=attached.revision;
        response.error=attached.error;return response;
    }
    TaskInputIntent intent=TaskInputIntent::ContinueTask;
    switch(request.command){
        case TaskCommandKind::Start:intent=TaskInputIntent::StartNewTask;break;
        case TaskCommandKind::Continue:intent=TaskInputIntent::ContinueTask;break;
        case TaskCommandKind::Amend:intent=TaskInputIntent::AmendRequirements;break;
        case TaskCommandKind::Suspend:intent=TaskInputIntent::SuspendTask;break;
        case TaskCommandKind::Cancel:intent=TaskInputIntent::CancelTask;break;
        case TaskCommandKind::Replan:intent=TaskInputIntent::ReplanTask;break;
        default:break;
    }
    const auto opened=orchestrator_.open_or_resume(turn,intent);
    if(!opened.ok){response.error=opened.error;return response;}
    response.task_revision=opened.task.revision;
    if(request.command==TaskCommandKind::Cancel&&controls_){
        const auto cancelled=controls_->cancel(turn.identity,turn.task_id,
            request.reason.empty()?"cancelled_by_user":request.reason,request.now_ms);
        if(!cancelled.ok){response.error="task_cancel_propagation_failed";return response;}
        response.payload["invocations_affected"]=cancelled.affected;
    }
    response.payload["task"]=encode(opened.task);
    attach_actions(response.payload,tasks_.load(turn.identity,turn.task_id));
    response.ok=true;return response;
}
} // namespace agent_framework::conversation
