#include <cassert>
#include <filesystem>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
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

    assert(supervisor.enqueue(request("s4","r4")).ok);
    SessionRunWorker fail_for_reconcile(supervisor,"worker-5",100,
        [](auto&){return WorkerExecutionResult{WorkerDisposition::Failed,"uncertain_effect"};});
    result=fail_for_reconcile.tick(610);assert(result.state==SupervisedRunState::Failed);
    auto reconcile_source=supervisor.load("t","r4");assert(reconcile_source);
    SessionRunCommand reconcile{"t","s4","r4","reconcile-1",SessionCommandKind::Reconcile,
        {{"receipt_id","receipt-1"}}};reconcile.expected_run_revision=reconcile_source->revision;
    assert(supervisor.enqueue_command(reconcile).ok);
    SessionRunWorker reconciling(supervisor,"worker-6",100,[](auto& context) {
        assert(context.commands.size()==1&&context.commands.front().kind==SessionCommandKind::Reconcile);
        return WorkerExecutionResult{WorkerDisposition::Completed,{}};});
    result=reconciling.tick(620);assert(result.state==SupervisedRunState::Completed);

    assert(supervisor.enqueue(request("s5","r5")).ok);
    SessionRunWorker park_for_escalation(supervisor,"worker-7",100,
        [](auto&){return WorkerExecutionResult{WorkerDisposition::AwaitingInput,"operator_required"};});
    result=park_for_escalation.tick(630);assert(result.state==SupervisedRunState::AwaitingInput);
    auto escalation_source=supervisor.load("t","r5");assert(escalation_source);
    SessionRunCommand escalate{"t","s5","r5","escalate-1",SessionCommandKind::Escalate,
        {{"assurance_tier","production_certification"}}};
    escalate.expected_run_revision=escalation_source->revision;
    assert(supervisor.enqueue_command(escalate).ok);
    SessionRunWorker escalating(supervisor,"worker-8",100,[](auto& context) {
        assert(context.commands.size()==1&&context.commands.front().kind==SessionCommandKind::Escalate);
        return WorkerExecutionResult{WorkerDisposition::Completed,{}};});
    result=escalating.tick(640);assert(result.state==SupervisedRunState::Completed);

    assert(supervisor.enqueue(request("s6","r6")).ok);
    SessionRunWorker park_for_queue(supervisor,"worker-9",100,
        [](auto&){return WorkerExecutionResult{WorkerDisposition::AwaitingInput,"more_work_pending"};});
    result=park_for_queue.tick(650);assert(result.state==SupervisedRunState::AwaitingInput);
    auto queue_source=supervisor.load("t","r6");assert(queue_source);
    SessionRunCommand queue{"t","s6","r6","queue-1",SessionCommandKind::Queue,
        {{"input","follow-up work"}}};queue.expected_run_revision=queue_source->revision;
    assert(supervisor.enqueue_command(queue).ok);
    SessionRunWorker queueing(supervisor,"worker-10",100,[](auto& context) {
        assert(context.commands.size()==1&&context.commands.front().kind==SessionCommandKind::Queue);
        return WorkerExecutionResult{WorkerDisposition::Completed,{}};});
    result=queueing.tick(660);assert(result.state==SupervisedRunState::Completed);

    // A client connection does not own execution.  Once three independent
    // Session Runs are durably enqueued, workers may complete them after the
    // request/client objects have gone out of scope without cross-Session data.
    {
        assert(supervisor.enqueue(request("parallel-s1","parallel-r1")).ok);
        assert(supervisor.enqueue(request("parallel-s2","parallel-r2")).ok);
        assert(supervisor.enqueue(request("parallel-s3","parallel-r3")).ok);
    }
    std::mutex observed_mutex;
    std::set<std::pair<std::string,std::string>> observed;
    std::vector<WorkerTickResult> ticks(3);
    std::vector<std::thread> workers;
    for(std::size_t i=0;i<ticks.size();++i) {
        workers.emplace_back([&,i] {
            SessionRunWorker concurrent(supervisor,"parallel-worker-"+std::to_string(i),100,
                [&](auto& context) {
                    assert(context.commands.size()==1);
                    assert(context.commands.front().kind==SessionCommandKind::Start);
                    std::lock_guard lock(observed_mutex);
                    observed.emplace(context.run.request.session_id,
                                     context.run.request.run_id);
                    return WorkerExecutionResult{WorkerDisposition::Completed,{}};
                });
            ticks[i]=concurrent.tick(700);
        });
    }
    for(auto& worker_thread:workers)worker_thread.join();
    const std::set<std::pair<std::string,std::string>> expected_observed({
        {"parallel-s1","parallel-r1"},
        {"parallel-s2","parallel-r2"},
        {"parallel-s3","parallel-r3"}});
    assert(observed==expected_observed);
    for(const auto& tick:ticks)assert(tick.claimed&&tick.executed&&tick.error.empty()&&
                                      tick.state==SupervisedRunState::Completed);
    for(int i=1;i<=3;++i) {
        const auto run=supervisor.load("t","parallel-r"+std::to_string(i));
        assert(run&&run->state==SupervisedRunState::Completed&&run->command_cursor==1);
    }
    std::filesystem::remove(path);
}
