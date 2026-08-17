#include <cassert>
#include <filesystem>
#include "agent/session/run_worker.hpp"

using namespace agent_framework::session;
static SessionRunRequest request(std::string session,std::string run) {
    SessionRunRequest r;r.tenant_id="t";r.organization_id="o";r.project_id="p";
    r.principal_id="u";r.provider_id="provider";r.session_id=std::move(session);
    r.run_id=std::move(run);r.command_id="start-"+r.run_id;r.payload={{"input","work"}};return r;
}
int main() {
    const auto path=(std::filesystem::temp_directory_path()/"agent-session-worker.sqlite").string();
    std::filesystem::remove(path);SQLiteSessionRunSupervisor supervisor(path);
    assert(supervisor.enqueue(request("s1","r1")).ok);
    bool heartbeat=false;
    SessionRunWorker worker(supervisor,"worker",100,[&](auto& context) {
        assert(context.run.request.session_id=="s1");assert(context.commands.size()==1);
        assert(context.commands.front().kind==SessionCommandKind::Start);heartbeat=context.heartbeat(50);
        return WorkerExecutionResult{WorkerDisposition::Completed,{}};});
    auto result=worker.tick(1);assert(result.claimed&&result.executed&&heartbeat&&result.error.empty());
    assert(supervisor.load("t","r1")->state==SupervisedRunState::Completed);
    assert(supervisor.load("t","r1")->command_cursor==1);
    assert(!worker.tick(200).claimed);

    assert(supervisor.enqueue(request("s2","r2")).ok);
    SessionRunWorker failing(supervisor,"worker-2",100,[](auto&)->WorkerExecutionResult {
        throw std::runtime_error("provider_failed");});
    result=failing.tick(300);assert(result.executed&&result.state==SupervisedRunState::Failed);
    assert(result.error=="provider_failed");
    auto failed=supervisor.load("t","r2");assert(failed&&failed->command_cursor==1);
    SessionRunCommand retry{"t","s2","r2","retry-1",SessionCommandKind::Retry,{{"reason","operator_retry"}}};
    retry.expected_run_revision=failed->revision;assert(supervisor.enqueue_command(retry).ok);
    SessionRunWorker retrying(supervisor,"worker-2b",100,[](auto& context) {
        assert(context.commands.size()==1&&context.commands.front().kind==SessionCommandKind::Retry);
        return WorkerExecutionResult{WorkerDisposition::Completed,{}};});
    result=retrying.tick(400);assert(result.state==SupervisedRunState::Completed);
    assert(supervisor.load("t","r2")->command_cursor==2);

    assert(supervisor.enqueue(request("s3","r3")).ok);
    SessionRunWorker parking(supervisor,"worker-3",100,[](auto& context) {
        assert(context.commands.size()==1&&context.commands.front().kind==SessionCommandKind::Start);
        return WorkerExecutionResult{WorkerDisposition::AwaitingInput,"decision_pending"};});
    result=parking.tick(500);assert(result.state==SupervisedRunState::AwaitingInput);
    auto parked=supervisor.load("t","r3");assert(parked&&parked->command_cursor==1);
    SessionRunCommand answer{"t","s3","r3","answer-1",SessionCommandKind::Steer,
        {{"decision_id","d1"}}};answer.expected_run_revision=parked->revision;
    assert(supervisor.enqueue_command(answer).ok);
    SessionRunWorker resumed(supervisor,"worker-4",100,[](auto& context) {
        assert(context.commands.size()==1&&context.commands.front().sequence==2&&
               context.commands.front().kind==SessionCommandKind::Steer);
        return WorkerExecutionResult{WorkerDisposition::Completed,{}};});
    result=resumed.tick(600);assert(result.state==SupervisedRunState::Completed);
    auto completed=supervisor.load("t","r3");assert(completed&&completed->command_cursor==2);
    std::filesystem::remove(path);
}
