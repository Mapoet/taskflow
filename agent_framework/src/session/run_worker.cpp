#include "agent/session/run_worker.hpp"
#include <algorithm>
#include <exception>
#include <stdexcept>

namespace agent_framework::session {

SessionRunWorker::SessionRunWorker(SQLiteSessionRunSupervisor& supervisor,
    std::string worker,std::uint64_t lease_ms,Executor executor)
    :supervisor_(supervisor),worker_id_(std::move(worker)),lease_ms_(lease_ms),
     executor_(std::move(executor)) {
    if(worker_id_.empty()||lease_ms_==0||!executor_)
        throw std::invalid_argument("session_run_worker_contract_invalid");
}

WorkerTickResult SessionRunWorker::tick(std::uint64_t now_ms) {
    WorkerTickResult result;std::string claim_error;
    auto run=supervisor_.claim_next(worker_id_,now_ms,lease_ms_,&claim_error);
    if(!run){result.error=std::move(claim_error);return result;}
    result.claimed=true;result.run_id=run->request.run_id;result.revision=run->revision;
    result.lease_epoch=run->lease_epoch;result.state=run->state;
    auto mutation=supervisor_.mark_running(run->request.tenant_id,run->request.run_id,
        worker_id_,run->lease_epoch,run->revision);
    if(!mutation.ok){result.error=mutation.error;return result;}
    run->state=SupervisedRunState::Running;run->revision=mutation.revision;
    result.revision=run->revision;result.state=run->state;
    WorkerExecutionContext context{*run,supervisor_.commands(run->request.tenant_id,
        run->request.run_id,run->command_cursor),{}};
    auto command_cursor=run->command_cursor;
    for(const auto& command:context.commands)
        command_cursor=std::max(command_cursor,command.sequence);
    context.heartbeat=[&,epoch=run->lease_epoch](std::uint64_t at) {
        const auto renewed=supervisor_.renew(run->request.tenant_id,run->request.run_id,
            worker_id_,epoch,run->revision,at+lease_ms_);
        if(!renewed.ok)return false;run->revision=renewed.revision;
        context.run.revision=renewed.revision;return true;};
    WorkerExecutionResult executed;
    try {executed=executor_(context);result.executed=true;}
    catch(const std::exception& error) {executed.disposition=WorkerDisposition::Failed;
        executed.diagnostic=error.what();result.executed=true;}
    RunSupervisorResult final;
    switch(executed.disposition) {
        case WorkerDisposition::AwaitingInput:
            final=supervisor_.await_input(run->request.tenant_id,run->request.run_id,
                worker_id_,run->lease_epoch,run->revision,now_ms+lease_ms_,command_cursor);
            result.state=SupervisedRunState::AwaitingInput;break;
        case WorkerDisposition::Completed:
            final=supervisor_.finish(run->request.tenant_id,run->request.run_id,
                worker_id_,run->lease_epoch,run->revision,SupervisedRunState::Completed,command_cursor);
            result.state=SupervisedRunState::Completed;break;
        case WorkerDisposition::Cancelled:
            final=supervisor_.finish(run->request.tenant_id,run->request.run_id,
                worker_id_,run->lease_epoch,run->revision,SupervisedRunState::Cancelled,command_cursor);
            result.state=SupervisedRunState::Cancelled;break;
        case WorkerDisposition::Failed:
            final=supervisor_.finish(run->request.tenant_id,run->request.run_id,
                worker_id_,run->lease_epoch,run->revision,SupervisedRunState::Failed,command_cursor);
            result.state=SupervisedRunState::Failed;break;
    }
    result.revision=final.revision;
    if(!final.ok)result.error=final.error;
    else if(!executed.diagnostic.empty())result.error=executed.diagnostic;
    return result;
}
}  // namespace agent_framework::session
