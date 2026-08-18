#include <cassert>
#include <filesystem>

#include "agent/session/run_supervisor.hpp"
#include "agent/internal/sqlite_utils.hpp"
#include <sqlite3.h>

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
        steer.expected_run_revision=running.revision;
        assert(supervisor.enqueue_command(steer).ok);assert(supervisor.enqueue_command(steer).ok);
        auto changed=steer;changed.payload={{"input","different"}};assert(!supervisor.enqueue_command(changed).ok);
        auto mismatch=steer;mismatch.command_id="bad-parent";mismatch.session_id="wrong";assert(!supervisor.enqueue_command(mismatch).ok);
        assert(supervisor.commands("tenant","r1").size()==2);
        assert(supervisor.finish("tenant","r1","worker-a",a->lease_epoch,running.revision,SupervisedRunState::Completed).ok);
        auto next=supervisor.claim_next("worker-d",110,100);assert(next&&next->request.run_id=="r4");
        auto running2=supervisor.mark_running("tenant","r2","worker-b",b->lease_epoch,b->revision);assert(running2.ok);
        auto waiting2=supervisor.await_input("tenant","r2","worker-b",b->lease_epoch,running2.revision,200);assert(waiting2.ok);
        std::string parked;auto unrelated=supervisor.claim_next("worker-new",201,100,&parked);
        assert(!unrelated||unrelated->request.run_id!="r2");
        SessionRunCommand resume{"tenant","s2","r2","resume-1",SessionCommandKind::Steer,{{"input","answer"}}};
        resume.expected_run_revision=waiting2.revision;assert(supervisor.enqueue_command(resume).ok);
        auto takeover=supervisor.claim_next("worker-new",201,100);assert(takeover&&takeover->request.run_id=="r2"&&takeover->lease_epoch==b->lease_epoch+1);
        assert(!supervisor.finish("tenant","r2","worker-b",b->lease_epoch,waiting2.revision,SupervisedRunState::Completed).ok);
        assert(supervisor.enqueue(request("fork-session","fork-parent","fork-parent-start")).ok);
        SessionRunCommand fork{"tenant","fork-session","fork-parent","fork-command",
            SessionCommandKind::Fork,{{"new_run_id","fork-child"},
                {"new_command_id","fork-child-start"},{"run_payload",{{"prompt","branched work"}}}}};
        fork.expected_run_revision=1;assert(supervisor.enqueue_command(fork).ok);
        const auto fork_child=supervisor.load("tenant","fork-child");
        assert(fork_child&&fork_child->request.session_id=="fork-session"&&
               fork_child->request.payload.at("prompt")=="branched work"&&
               fork_child->state==SupervisedRunState::Queued);
        const auto child_commands=supervisor.commands("tenant","fork-child");
        assert(child_commands.size()==1&&child_commands.front().kind==SessionCommandKind::Start);
        auto bad_fork=fork;bad_fork.command_id="bad-fork";bad_fork.payload=nlohmann::json::object();
        assert(!supervisor.enqueue_command(bad_fork).ok);
    }
    {
        SQLiteSessionRunSupervisor reopened(path);auto run=reopened.load("tenant","r2");
        assert(run&&run->lease_owner=="worker-new"&&run->lease_epoch==2);
    }
    const auto recovery_path=(std::filesystem::temp_directory_path()/"agent-session-run-recovery.sqlite").string();
    std::filesystem::remove(recovery_path);
    std::uint64_t stale_epoch=0,stale_revision=0;
    {
        SQLiteSessionRunSupervisor before_restart(recovery_path);
        assert(before_restart.enqueue(request("restart-session","restart-run","restart-start")).ok);
        auto claimed=before_restart.claim_next("stale-worker",100,50);assert(claimed);
        stale_epoch=claimed->lease_epoch;
        auto running=before_restart.mark_running("tenant","restart-run","stale-worker",
                                                 claimed->lease_epoch,claimed->revision);
        assert(running.ok);stale_revision=running.revision;
    }
    {
        SQLiteSessionRunSupervisor after_restart(recovery_path);
        auto takeover=after_restart.claim_next("recovery-worker",151,50);assert(takeover);
        assert(takeover->request.run_id=="restart-run"&&takeover->lease_epoch==stale_epoch+1);
        const auto stale_finish=after_restart.finish("tenant","restart-run","stale-worker",
            stale_epoch,stale_revision,SupervisedRunState::Completed);
        assert(!stale_finish.ok&&stale_finish.error=="lease_fence_or_revision_conflict");
        auto running=after_restart.mark_running("tenant","restart-run","recovery-worker",
                                                takeover->lease_epoch,takeover->revision);
        assert(running.ok);
        assert(after_restart.finish("tenant","restart-run","recovery-worker",
            takeover->lease_epoch,running.revision,SupervisedRunState::Completed,1).ok);
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
    const auto legacy_path=(std::filesystem::temp_directory_path()/"agent-session-run-legacy.sqlite").string();
    std::filesystem::remove(legacy_path);sqlite3* legacy=nullptr;
    assert(sqlite3_open(legacy_path.c_str(),&legacy)==SQLITE_OK);
    agent_framework::internal::sqlite::exec(legacy,
        "CREATE TABLE supervised_session_runs(tenant TEXT NOT NULL,organization_id TEXT NOT NULL,"
        "project_id TEXT NOT NULL,principal_id TEXT NOT NULL,provider_id TEXT NOT NULL,session_id TEXT NOT NULL,"
        "run_id TEXT NOT NULL,command_id TEXT NOT NULL,payload_json TEXT NOT NULL,state TEXT NOT NULL,"
        "revision INTEGER NOT NULL,lease_epoch INTEGER NOT NULL,lease_owner TEXT NOT NULL,"
        "lease_expires_at_ms INTEGER NOT NULL,created_at TEXT NOT NULL,updated_at TEXT NOT NULL,"
        "PRIMARY KEY(tenant,run_id),UNIQUE(tenant,session_id,command_id));");
    sqlite3_close(legacy);
    {SQLiteSessionRunSupervisor migrated(legacy_path);
        assert(migrated.enqueue(request("legacy-session","legacy-run","legacy-start")).ok);
        auto run=migrated.load("tenant","legacy-run");assert(run&&run->command_cursor==0);}
    std::filesystem::remove(path);std::filesystem::remove(quota_path);
    std::filesystem::remove(recovery_path);
    std::filesystem::remove(legacy_path);
}
