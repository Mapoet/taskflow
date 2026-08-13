#include <agent/tool_runtime/long_task_workflow.hpp>
#include <agent/internal/platform_io.hpp>
#include <cassert>
#include <filesystem>
using namespace agent_framework;
using namespace agent_framework::tool_runtime;
namespace
{
    struct Inputs : PlanNodeInputRepository
    {
        std::map<std::string, PlanNodeExecutionDescriptor> values;
        std::optional<PlanNodeExecutionDescriptor> descriptor(
            const contracts::ContractIdentity &, std::string_view, std::string_view node) override
        { auto found=values.find(std::string(node));return found==values.end()?std::nullopt:std::optional(found->second); }
    };
    struct Executor : ProductionPlanNodeExecutor
    {
        int calls{0}; std::string id()const override{return "typed";}std::string revision()const override{return "v1";}
        PlanNodeExecutorOrigin origin()const noexcept override{return PlanNodeExecutorOrigin::Production;}
        PlanNodeExecutionResult start(const LongTaskCheckpoint&w,const planning::PlanNode&n,const PlanNodeExecutionDescriptor&)override
        {++calls;return{true,w.workflow_id+":"+n.node_id,"external",{}};}
    };
    struct Model : LongTaskCognitionModel
    {
        int calls = 0;
        LongTaskDecision invoke(const LongTaskCheckpoint &, const planning::ExecutionPlan &, const ObservationBatch &) override
        {
            ++calls;
            LongTaskDecision d;
            d.kind = LongTaskDecisionKind::ContinueWaiting;
            d.manifest.invocation_id = "llm-" + std::to_string(calls);
            return d;
        }
    };
    planning::ExecutionPlan plan()
    {
        planning::ExecutionPlan p;
        p.metadata.identity.tenant_id = "t";
        p.metadata.identity.task_id = "task";
        p.metadata.identity.plan_id = "plan";
        p.plan_revision = 1;
        p.nodes = {planning::PlanNode{"a", "collect"}, planning::PlanNode{"b", "analyze"}};
        p.nodes[1].dependencies = {"a"};
        p.budget.wall_time_ms = 1000;
        p.budget.tool_calls = 10;
        p.budget.token_budget = 100;
        return p;
    }
    LongRunningToolInvocation invocation()
    {
        LongRunningToolInvocation v;
        v.metadata.identity.tenant_id = "t";
        v.metadata.identity.task_id = "task";
        v.metadata.identity.run_id = "run";
        v.invocation_id = "inv";
        v.conversation_id = "c";
        v.turn_id = "turn";
        v.tool_call_id = "call";
        v.tool_name = "tool";
        v.tool_contract_revision = "v1";
        v.deployment_revision = "d";
        v.tool_generation = "g";
        v.input_digest = "sha256:i";
        v.created_at = v.updated_at = "now";
        return v;
    }
    void advance(SQLiteInvocationStore &s, LongRunningToolInvocation &v, InvocationState state, std::string type, bool gain = false)
    {
        auto prior = v.revision;
        v.revision++;
        v.state = state;
        InvocationEvent e;
        e.event_type = std::move(type);
        e.information_gain = gain;
        assert(s.commit({v, prior, e, {}, {}, {}}));
    }
}
int main()
{
    namespace fs = std::filesystem;
    auto root = fs::temp_directory_path() / ("long-task-" + std::to_string(internal::current_process_id()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    auto path = (root / "state.sqlite3").string();
    SQLiteLongTaskStore store(path);
    SQLiteInvocationStore inv(path);
    planning::InMemoryPlanStore plans;
    auto p = plan();
    assert(plans.create(p));
    auto v = invocation();
    assert(inv.create(v));
    advance(inv, v, InvocationState::Admitted, "admit");
    advance(inv, v, InvocationState::Queued, "queue");
    advance(inv, v, InvocationState::Leased, "lease");
    advance(inv, v, InvocationState::Running, "run");
    Model model;
    LongTaskWorkflow workflow(store, plans, inv, model, ObservationClassifier({100, 0.8}));
    LongTaskCheckpoint c;
    c.metadata = p.metadata;
    c.metadata.identity.run_id = "run";
    c.workflow_id = "workflow";
    c.plan_revision = 1;
    c.plan_digest = planning::encode(p).at("canonical_digest");
    c.fencing_token = 1;
    c.created_at = c.updated_at = "now";
    c.limit = {1000, 10, 10, 100, 1};
    c.watches["inv"] = {"inv", v.progress_sequence, "", 1};
    assert(workflow.start(c, p));
    auto first = workflow.step("workflow", 50);
    assert(first.error.empty() && !first.llm_invoked && model.calls == 0 && first.ready_nodes == std::vector<std::string>{"a"});
    Inputs inputs;
    PlanNodeExecutionDescriptor descriptor;descriptor.metadata=p.metadata;descriptor.plan_digest=c.plan_digest;
    descriptor.node_id="a";descriptor.executor_id="typed";descriptor.executor_revision="v1";
    descriptor.descriptor_digest=contracts::canonical_digest({{"plan_digest",descriptor.plan_digest},{"node_id",descriptor.node_id},{"executor_id",descriptor.executor_id},{"executor_revision",descriptor.executor_revision},{"input",descriptor.input},{"granted_capabilities",descriptor.granted_capabilities},{"approval_decision_id",descriptor.approval_decision_id},{"side_effecting",descriptor.side_effecting}}).value_or("");
    inputs.values["a"]=descriptor;PlanNodeExecutorRegistry registry;auto executor=std::make_shared<Executor>();assert(registry.register_executor(executor));
    LongTaskDispatcher dispatcher(store,plans,inputs,registry);auto dispatched=dispatcher.dispatch(first);
    assert(dispatched.error.empty()&&executor->calls==1&&dispatched.checkpoint.nodes.at("a").state==PlanNodeState::Running);
    auto repeated=dispatcher.dispatch(first);assert(executor->calls==1);
    // One thousand routine heartbeats must not invoke cognition.
    for (int i = 0; i < 1000; ++i)
    {
        advance(inv, v, i % 2 ? InvocationState::Progressing : InvocationState::Checkpointed, "invocation_heartbeat", false);
        auto x = workflow.step("workflow", 50);
        assert(!x.llm_invoked);
    }
    assert(model.calls == 0);
    advance(inv, v, v.state == InvocationState::Progressing ? InvocationState::Checkpointed : InvocationState::Progressing, "partial_result", true);
    auto meaningful = workflow.step("workflow", 51);
    assert(meaningful.llm_invoked && model.calls == 1);
    auto duplicate = workflow.step("workflow", 52);
    assert(!duplicate.llm_invoked && model.calls == 1);
    InvocationEventStreamHub hub(inv);
    const auto timed = workflow.wait_and_step("workflow", hub,
                                              std::chrono::milliseconds(1), 52);
    assert(timed.error.empty() && !timed.llm_invoked && model.calls == 1);
    run::SQLiteRunStore runs((root / "run.sqlite3").string());
    run::RunCheckpoint run_checkpoint;
    run_checkpoint.metadata = p.metadata;
    run_checkpoint.metadata.identity.run_id = "run";
    run_checkpoint.state = run::RunState::Running;
    assert(runs.create(run_checkpoint));
    LongTaskTimerWorker timers(runs, workflow, dispatcher, "timer-worker", 100);
    auto timer_checkpoint = store.load("workflow");
    assert(timer_checkpoint && timers.schedule(*timer_checkpoint, 60, "stall_check"));
    assert(timers.run_due(59) == 0);
    assert(timers.run_due(60) == 1);
    auto loaded = store.load("workflow");
    auto head = inv.event_head("inv");
    assert(loaded && loaded->watches.at("inv").cursor == head);
    SQLiteLongTaskStore restarted(path);
    assert(restarted.load("workflow")->revision == loaded->revision);
    assert(restarted.recoverable(10).size() == 1);
    loaded->nodes["a"].state = PlanNodeState::CompletedCandidate;
    auto dag = schedule_dag(p, *loaded);
    assert(dag.valid && dag.ready == std::vector<std::string>{"b"});
    auto bad = p;
    bad.plan_revision = 2;
    bad.parent_plan_digest = planning::encode(p).at("canonical_digest");
    bad.nodes.erase(bad.nodes.begin());
    std::string error;
    assert(!validate_revision(p, bad, *loaded, &error));
    auto cyclic = p;
    cyclic.nodes[0].dependencies = {"b"};
    assert(!schedule_dag(cyclic, *loaded).valid);
    auto e = store.events("workflow", 0);
    assert(!e.empty());
    for (size_t i = 1; i < e.size(); ++i)
        assert(e[i].previous_digest == e[i - 1].event_digest);
    auto stale = *loaded;
    const auto stale_revision = stale.revision;
    stale.revision++;
    stale.fencing_token++;
    assert(!store.commit(stale, stale_revision,
                         {stale.workflow_id, "stale_owner", 0, stale.revision,
                          stale.fencing_token, {}, {}, {}, "now"}));
    fs::remove_all(root, ec);
}
