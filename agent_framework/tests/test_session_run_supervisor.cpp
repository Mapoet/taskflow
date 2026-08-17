#include <cassert>
#include <filesystem>

#include "agent/session/run_supervisor.hpp"

using namespace agent_framework::session;

SessionRunRequest request(std::string session,std::string run,std::string command,
                          std::string principal="principal"){
    SessionRunRequest v;v.tenant_id="tenant";v.organization_id="org";
    v.project_id="project";v.principal_id=std::move(principal);v.provider_id="provider";
    v.session_id=std::move(session);v.run_id=std::move(run);v.command_id=std::move(command);
    v.payload={{"prompt","work"}};return v;
}
int main(){
    const auto path=(std::filesystem::temp_directory_path()/"agent-session-run-supervisor.sqlite").string();std::filesystem::remove(path);
    {
        SQLiteSessionRunSupervisor supervisor(path);
        assert(supervisor.enqueue(request("s1","r1","c1")).ok);
        assert(supervisor.enqueue(request("s1","r1","c1")).ok);
        assert(supervisor.enqueue(request("s2","r2","c2")).ok);
        assert(supervisor.enqueue(request("s3","r3","c3")).ok);
        assert(supervisor.enqueue(request("s1","r4","c4")).ok);
        auto a=supervisor.claim_next("worker-a",100,100);assert(a&&a->request.run_id=="r1");
        auto b=supervisor.claim_next("worker-b",100,100);assert(b&&b->request.run_id=="r2");
        auto c=supervisor.claim_next("worker-c",100,100);assert(c&&c->request.run_id=="r3");
        std::string none;assert(!supervisor.claim_next("worker-d",100,100,&none)&&none=="no_eligible_run");
        auto running=supervisor.mark_running("tenant","r1","worker-a",a->lease_epoch,a->revision);assert(running.ok);
        SessionRunCommand steer{"tenant","s1","r1","steer-1",SessionCommandKind::Steer,{{"input","refine"}}};
        assert(supervisor.enqueue_command(steer).ok);assert(supervisor.enqueue_command(steer).ok);
        auto changed=steer;changed.payload={{"input","different"}};assert(!supervisor.enqueue_command(changed).ok);
        auto mismatch=steer;mismatch.command_id="bad-parent";mismatch.session_id="wrong";assert(!supervisor.enqueue_command(mismatch).ok);
        assert(supervisor.commands("tenant","r1").size()==2);
        assert(supervisor.finish("tenant","r1","worker-a",a->lease_epoch,running.revision,SupervisedRunState::Completed).ok);
        auto next=supervisor.claim_next("worker-d",110,100);assert(next&&next->request.run_id=="r4");
        auto running2=supervisor.mark_running("tenant","r2","worker-b",b->lease_epoch,b->revision);assert(running2.ok);
        auto waiting2=supervisor.await_input("tenant","r2","worker-b",b->lease_epoch,running2.revision,200);assert(waiting2.ok);
        auto takeover=supervisor.claim_next("worker-new",201,100);assert(takeover&&takeover->request.run_id=="r2"&&takeover->lease_epoch==b->lease_epoch+1);
        assert(!supervisor.finish("tenant","r2","worker-b",b->lease_epoch,waiting2.revision,SupervisedRunState::Completed).ok);
    }
    {
        SQLiteSessionRunSupervisor reopened(path);auto run=reopened.load("tenant","r2");
        assert(run&&run->lease_owner=="worker-new"&&run->lease_epoch==2);
    }
    const auto quota_path=(std::filesystem::temp_directory_path()/"agent-session-run-quota.sqlite").string();std::filesystem::remove(quota_path);
    {
        RunSupervisorQuota quota;quota.principal_active=1;
        SQLiteSessionRunSupervisor supervisor(quota_path,quota);
        assert(supervisor.enqueue(request("q1","qr1","qc1","same")).ok);
        assert(supervisor.enqueue(request("q2","qr2","qc2","same")).ok);
        assert(supervisor.claim_next("worker",1,100));std::string error;
        assert(!supervisor.claim_next("worker",1,100,&error)&&error=="no_eligible_run");
    }
    std::filesystem::remove(path);std::filesystem::remove(quota_path);
}
