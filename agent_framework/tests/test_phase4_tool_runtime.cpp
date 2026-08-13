#include <agent/tool_runtime/worker_runtime.hpp>
#include <agent/tool_runtime/state_machine.hpp>
#include <agent/tool_runtime/progress_protocol.hpp>
#include <agent/tool_runtime/event_stream.hpp>
#include <agent/ui/live_operations_projection.hpp>
#include <agent/distributed/object_store.hpp>
#include <agent/internal/platform_io.hpp>
#include <cassert>
#include <filesystem>
using namespace agent_framework::tool_runtime;
static LongRunningToolInvocation invocation()
{
    LongRunningToolInvocation v;
    v.metadata.identity.tenant_id = "tenant";
    v.metadata.identity.task_id = "task";
    v.metadata.identity.run_id = "run";
    v.invocation_id = "inv-1";
    v.conversation_id = "conversation";
    v.turn_id = "turn";
    v.tool_call_id = "call";
    v.tool_name = "tool";
    v.tool_contract_revision = "tool-v1";
    v.deployment_revision = "deploy-v1";
    v.tool_generation = "g1";
    v.input_digest = "sha256:" + std::string(64, '1');
    v.created_at = "now";
    v.updated_at = "now";
    v.idempotent = true;
    return v;
}
int main()
{
    auto base = std::filesystem::temp_directory_path() / ("tool-runtime-" + std::to_string(agent_framework::internal::current_process_id()));
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    std::filesystem::create_directories(base);
    auto path = (base / "control.sqlite3").string();
    SQLiteInvocationStore store(path);
    agent_framework::distributed::SQLiteDurableQueue queue(path);
    agent_framework::distributed::SQLiteWorkerRegistry workers(path);
    agent_framework::distributed::WorkerRecord wr{"worker-a", "instance-a", "sha256:caps", 0, 100};
    auto gen = workers.register_worker(wr);
    assert(gen && *gen == 1);
    assert(workers.set_quota("tenant", 1));
    LeaseWorkerRuntime runtime(store, queue, workers, {"worker-a", "instance-a", 1}, 50);
    auto v = invocation();
    assert(decode_invocation(encode(v)));
    auto future = encode(v);
    future["kind"] = "agent.long_running_tool_invocation/v2";
    future["canonical_digest"] = *agent_framework::contracts::embedded_digest(future);
    assert(!decode_invocation(future));
    auto enqueued = runtime.enqueue(v);
    if (!enqueued) throw std::runtime_error(enqueued.error);
    auto claimed = runtime.claim("tenant", 100);
    assert(claimed && claimed->invocation.state == InvocationState::Running && claimed->queue_lease.fencing_token == 1);
    auto active = store.load("inv-1");
    assert(active && active->revision == 5);
    SQLiteInvocationStore reader(path);
    InvocationEventStreamHub remote_hub(reader);
    auto remote = remote_hub.subscribe("inv-1", reader.event_head("inv-1"), 8);
    assert(remote.subscription && remote.head_sequence == 4);
    InvocationEventStreamHub hub(store);
    auto slow = hub.subscribe("inv-1", store.event_head("inv-1"), 1);
    assert(slow.subscription);
    ProgressCheckpoint progress{1, 0.5, "half", "cas://checkpoint", "sha256:checkpoint", "now", true};
    auto next = *active;
    next.revision++;
    next.state = InvocationState::Progressing;
    next.progress_sequence = 1;
    InvocationEvent event;
    event.event_type = "invocation_progress";
    event.fencing_token = 1;
    event.payload = {{"fraction", 0.5}};
    event.information_gain = true;
    PartialResultRef partial{1, "log", "cas://partial", "sha256:partial", "text/plain", 12, true};
    assert(store.commit({next, active->revision, event, progress, partial, {}}));
    auto ui_subscription = remote_hub.subscribe("inv-1", 4, 8);
    assert(ui_subscription.subscription);
    agent_framework::LiveOperationsProjection ui_projection(
        agent_framework::LiveOperationsIdentity{"tenant", "run", "task"});
    assert(ui_projection.consume_invocations(*ui_subscription.subscription,
        std::chrono::milliseconds(500)) == 1);
    assert(ui_projection.snapshot().summary.find("50.000000%") != std::string::npos);
    InvocationEvent delivered;
    assert(remote.subscription->next(delivered, std::chrono::milliseconds(500)) ==
           InvocationSubscriptionRead::Event && delivered.sequence == 5);
    assert(store.latest_progress("inv-1")->sequence == 1 && store.partial_results("inv-1").size() == 1);
    assert(store.verify_history("inv-1").valid);
    auto durable_events = store.events("inv-1");
    auto projected = project_runtime_event(next, durable_events.back());
    assert(projected && projected->tool_call_id == "call" &&
           projected->payload.at("invocation_sequence") == durable_events.back().sequence);
    InvocationEvent heartbeat; heartbeat.invocation_id="inv-1"; heartbeat.sequence=1;
    heartbeat.event_type="invocation_heartbeat"; heartbeat.durability=InvocationEventDurability::Ephemeral;
    assert(classify_progress(heartbeat)==ProgressDisposition::AggregateEphemeral);
    heartbeat.information_gain=true;
    assert(classify_progress(heartbeat)==ProgressDisposition::WakeOrchestrator);
    auto stale = next;
    stale.revision++;
    stale.state = InvocationState::Checkpointed;
    InvocationEvent stale_event;
    stale_event.event_type = "checkpoint";
    stale_event.fencing_token = 99;
    assert(store.commit({stale, next.revision, stale_event, {}, {}, {}}).status == InvocationStoreStatus::FencingRejected);
    claimed->invocation = *store.load("inv-1");
    InvocationReceipt receipt{"sha256:result", "sha256:effect", "", "now", true};
    assert(runtime.complete(*claimed, receipt, 150));
    assert(queue.inspect("inv-1")->state == agent_framework::distributed::QueueState::Completed);
    {
        SQLiteInvocationStore reopened(path);
        assert(reopened.load("inv-1")->state == InvocationState::CompletedCandidate);
        assert(reopened.events("inv-1").size() == 6);
        assert(reopened.verify_history("inv-1").valid);
        auto scoped = reopened.query({"tenant", "conversation", "run", "call", 10});
        assert(scoped.size() == 1 && scoped.front().invocation_id == "inv-1");
        assert(reopened.events("inv-1", 0, 2).size() == 2);
    }
    auto published = store.events("inv-1", 4, 2);
    assert(published.size() == 2);
    hub.publish(published[0]);
    hub.publish(published[1]);
    assert(slow.subscription->next(delivered, std::chrono::milliseconds(0)) ==
           InvocationSubscriptionRead::Event);
    assert(slow.subscription->next(delivered, std::chrono::milliseconds(0)) ==
           InvocationSubscriptionRead::Overflow);

    agent_framework::distributed::FilesystemObjectStore objects(base / "objects");
    auto retained = store.apply_retention("inv-1", {2, 0, false}, objects);
    assert(retained.applied && retained.events == 4 && !retained.archive_digest.empty());
    assert(store.event_head("inv-1") == 6 && store.event_retention_floor("inv-1") == 5);
    assert(store.events("inv-1").size() == 2 && store.verify_history("inv-1").valid);
    assert(store.latest_progress("inv-1") && store.partial_results("inv-1").size() == 1);
    auto expired = hub.subscribe("inv-1", 0, 8);
    assert(!expired.subscription && expired.error == "cursor_expired");
    auto retained_replay = hub.subscribe("inv-1", 4, 8);
    assert(retained_replay.subscription && retained_replay.head_sequence == 6);
    assert(retained_replay.subscription->next(delivered, std::chrono::milliseconds(0)) ==
           InvocationSubscriptionRead::Event && delivered.sequence == 5);
    auto takeover_path=(base/"takeover.sqlite3").string();
    SQLiteInvocationStore takeover_store(takeover_path);
    agent_framework::distributed::SQLiteDurableQueue takeover_queue(takeover_path);
    agent_framework::distributed::SQLiteWorkerRegistry takeover_workers(takeover_path);
    auto wa=takeover_workers.register_worker({"wa","ia","sha256:a",0,0});
    auto wb=takeover_workers.register_worker({"wb","ib","sha256:b",0,0});
    assert(wa&&wb&&takeover_workers.set_quota("tenant",1));
    LeaseWorkerRuntime ra(takeover_store,takeover_queue,takeover_workers,{"wa","ia",*wa},10);
    LeaseWorkerRuntime rb(takeover_store,takeover_queue,takeover_workers,{"wb","ib",*wb},10);
    auto tv=invocation();tv.invocation_id="takeover";tv.tool_call_id="takeover-call";
    assert(ra.enqueue(tv));auto ca=ra.claim("tenant",100);assert(ca&&ca->queue_lease.fencing_token==1);
    auto cb=rb.claim("tenant",110);assert(cb&&cb->queue_lease.fencing_token==2&&cb->invocation.state==InvocationState::Running);
    auto old=*takeover_store.load("takeover");old.revision++;old.state=InvocationState::Progressing;
    InvocationEvent late;late.event_type="late_progress";late.fencing_token=1;
    assert(takeover_store.commit({old,old.revision-1,late,{},{},{}}).status==InvocationStoreStatus::FencingRejected);
    auto takeover_events=takeover_store.events("takeover");bool saw_orphan=false;
    for(const auto&e:takeover_events)saw_orphan=saw_orphan||e.event_type=="invocation_orphaned";
    assert(saw_orphan&&takeover_store.verify_history("takeover").valid);
    std::filesystem::remove_all(base, ec);
}
