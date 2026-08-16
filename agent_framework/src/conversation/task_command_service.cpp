#include "agent/conversation/task_command_service.hpp"

namespace agent_framework::conversation {
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
    if(request.command==TaskCommandKind::Status||request.command==TaskCommandKind::Output){
        response.read_only=true;
        auto task=tasks_.load(turn.identity,turn.task_id);
        if(!task){response.error="task_not_found";return response;}
        response.task_revision=task->revision;
        response.payload=controls_?controls_->status(turn.identity,turn.task_id)
            :nlohmann::json{{"found",true},{"task",encode(*task)}};
        if(request.command==TaskCommandKind::Output&&response.payload.contains("runs")){
            nlohmann::json output=nlohmann::json::array();
            for(const auto& run:response.payload["runs"])
                for(const auto& invocation:run.value("invocations",nlohmann::json::array()))
                    if(invocation.contains("partial_output"))output.push_back(invocation["partial_output"]);
            response.payload={{"task_id",turn.task_id},{"outputs",std::move(output)},
                              {"task_revision",task->revision}};
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
    response.ok=true;return response;
}
} // namespace agent_framework::conversation
