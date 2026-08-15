#include <agent/agent_template/compiler.hpp>
#include <cassert>
#include <iostream>
#include <filesystem>
#include <agent/approval/store.hpp>
using namespace agent_framework;
using namespace agent_framework::agent_template;
namespace { class Nested final:public NestedWorkflowPort{public:RunnerResult execute(const RunnerRequest&r)override{RunnerResult x;x.ok=true;x.output={{"nested",true}};x.receipt.invocation_id=r.invocation.invocation_id;x.receipt.node_id=r.node.node_id;x.receipt.runner=SkillRunnerKind::NestedWorkflow;x.receipt.terminal_state=RunnerLifecycleState::Succeeded;return x;}bool cancel(std::string_view)override{return true;}}; }
int main()
{
    auto registry = std::make_shared<SkillRunnerRegistry>();
    for (auto kind : {SkillRunnerKind::InlinePrompt, SkillRunnerKind::LocalCapability, SkillRunnerKind::SandboxedProcess, SkillRunnerKind::Cli, SkillRunnerKind::Mcp, SkillRunnerKind::ChildAgent, SkillRunnerKind::NestedWorkflow, SkillRunnerKind::HumanApproval})
        assert(registry->register_runner(std::make_shared<CallbackSkillRunner>(kind, [](const RunnerRequest &r)
                                                                               {auto out=r.input;out["executed_by"]=to_string(r.node.runner);if(out.contains("value"))out["value"]=out["value"].get<int>()+1;return out; })));
    SkillCollaborationPlan p;
    p.plan_id = "pipeline";
    p.budget.max_parallelism = 2;
    SkillPlanNode a;
    a.node_id = "a";
    a.resolved_skill_id = "skill-a";
    a.runner = SkillRunnerKind::LocalCapability;
    SkillPlanNode b = a;
    b.node_id = "b";
    b.runner = SkillRunnerKind::Mcp;
    b.input_mapping = {{"value", {{"from", "a"}, {"path", "/value"}}}};
    SkillPlanNode c = a;
    c.node_id = "c";
    c.runner = SkillRunnerKind::ChildAgent;
    c.input_mapping = b.input_mapping;
    p.nodes = {a, b, c};
    p.edges = {{"a", "b", {}}, {"a", "c", {}}};
    p.output_assembly = {{"from", "b"}};
    AgentTemplateInvocation invocation;
    invocation.invocation_id = "inv";
    ActiveSkillSession session;
    SkillWorkflowCompiler compiler(registry);
    auto result = compiler.execute(p, invocation, session, {{"value", 1}});
    assert(result.ok);
    assert(result.node_outputs.at("a").at("value") == 2);
    assert(result.node_outputs.at("b").at("value") == 3);
    assert(result.node_outputs.at("c").at("value") == 3);
    assert(result.receipts.size() == 3);
    assert(result.events.size() == 12);
    assert(result.output.at("executed_by") == "mcp");
    auto literals=p;literals.nodes[0].input_mapping={{"value",{{"value",41}}},{"label",{{"value","fixed"}}}};
    auto literal_result=compiler.execute(literals,invocation,session,{{"value",1}});
    assert(literal_result.ok&&literal_result.node_outputs.at("a").at("value")==42&&literal_result.node_outputs.at("a").at("label")=="fixed");
    auto ambiguous=literals;ambiguous.nodes[0].input_mapping["value"]={{"value",41},{"from","$input"}};
    assert(SkillWorkflowCompiler(registry).execute(ambiguous,invocation,session,{{"value",1}}).error_code=="input_mapping_failed");
    auto missing = p;
    missing.nodes[1].runner = SkillRunnerKind::HumanApproval;
    auto limited = std::make_shared<SkillRunnerRegistry>();
    limited->register_runner(registry->resolve(SkillRunnerKind::LocalCapability));
    assert(!SkillWorkflowCompiler(limited).execute(missing, invocation, session, {{"value", 1}}).ok);
    auto conditional=p;conditional.edges[0].condition="false";
    auto conditional_result=compiler.execute(conditional,invocation,session,{{"value",1}});
    assert(conditional_result.ok&&conditional_result.node_outputs.at("b").at("skipped"));
    auto schema=p;schema.nodes[0].output.schema={{"type","object"},{"properties",{{"value",{{"type","string"}}}}},{"required",{"value"}}};
    assert(compiler.execute(schema,invocation,session,{{"value",1}}).error_code=="output_contract_failed");
    auto retry=p;retry.nodes[0].effect=EffectClass::Write;retry.nodes[0].max_attempts=2;retry.nodes[0].idempotency_key.clear();
    assert(compiler.execute(retry,invocation,session,{{"value",1}}).error_code=="idempotency_key_required");
    auto production=std::make_shared<SkillRunnerRegistry>(true);
    assert(!production->register_runner(std::make_shared<CallbackSkillRunner>(SkillRunnerKind::LocalCapability,[](const RunnerRequest&){return json::object();})));
    auto bus=std::make_shared<ToolBus>();
    ToolMeta echo_meta;echo_meta.name="Echo";echo_meta.description="echo";echo_meta.schema={{"type","object"},{"properties",{{"value",{{"type","integer"}}}}}};
    bus->register_local_tool("Echo",[](const json& in){return json{{"value",in.value("value",0)}};},echo_meta);
    auto production_runners=build_production_toolbus_runners(bus);
    assert(production_runners->resolve(SkillRunnerKind::LocalCapability));
    assert(production_runners->resolve(SkillRunnerKind::SandboxedProcess));
    assert(production_runners->resolve(SkillRunnerKind::Cli));
    assert(production_runners->resolve(SkillRunnerKind::Mcp));
    assert(!production_runners->resolve(SkillRunnerKind::ChildAgent));
    RunnerRequest tool_request;tool_request.invocation.invocation_id="prod";tool_request.node.node_id="echo";
    tool_request.session.effective_permissions.tools={"Echo"};tool_request.input={{"tool","Echo"},{"arguments",{{"value",7}}}};
    auto tool_result=production_runners->resolve(SkillRunnerKind::LocalCapability)->run(tool_request);
    assert(tool_result.ok&&tool_result.output.at("value")==7&&tool_result.events.size()==4);
    ToolMeta failed_meta;failed_meta.name="FailedProcess";failed_meta.schema={{"type","object"}};
    bus->register_local_tool("FailedProcess",[](const json&){return json{{"exit_code",2},{"stderr","failed"}};},failed_meta);
    tool_request.session.effective_permissions.tools={"FailedProcess"};tool_request.input={{"tool","FailedProcess"},{"arguments",json::object()}};
    auto failed_process=production_runners->resolve(SkillRunnerKind::LocalCapability)->run(tool_request);
    assert(!failed_process.ok&&failed_process.error_code=="tool_exit_nonzero"&&failed_process.receipt.terminal_state==RunnerLifecycleState::Failed);
    auto child=std::make_shared<LocalChildTaskBackend>([](const ChildTaskRequest&r){ChildTaskResult x;x.status=ChildTaskStatus::Completed;x.child_id=r.child_id;x.run_id=r.run_id;x.outputs={{"task_completion_verified",true},{"completion_authority","task_closure_controller"}};return x;});
    auto approval_path=(std::filesystem::temp_directory_path()/"agent-template-approval.sqlite3").string();std::filesystem::remove(approval_path);
    approval::SQLiteApprovalStore approval_store(approval_path);
    auto complete_runners=build_production_runners({bus,child,std::make_shared<Nested>(),&approval_store});
    assert(complete_runners->resolve(SkillRunnerKind::ChildAgent)&&complete_runners->resolve(SkillRunnerKind::NestedWorkflow)&&complete_runners->resolve(SkillRunnerKind::HumanApproval));
    approval::ApprovalRequest stored;stored.metadata.identity.tenant_id="tenant";stored.metadata.identity.task_id="task";stored.approval_id="pending";stored.requester_id="agent";stored.policy_revision="p1";stored.plan_digest="plan";stored.arguments_digest="sha256:args";
    assert(approval_store.put_request(stored));
    RunnerRequest approval_request=tool_request;approval_request.invocation.plan_digest="plan";approval_request.input={{"approval_id","pending"},{"arguments_digest","sha256:args"},{"policy_revision","p1"}};
    auto awaiting=complete_runners->resolve(SkillRunnerKind::HumanApproval)->run(approval_request);
    assert(!awaiting.ok&&awaiting.error_code=="awaiting_approval"&&awaiting.receipt.terminal_state==RunnerLifecycleState::Waiting);
    SkillCollaborationPlan approval_plan;approval_plan.plan_id="approval-plan";SkillPlanNode approval_node;
    approval_node.node_id="approval";approval_node.runner=SkillRunnerKind::HumanApproval;approval_plan.nodes={approval_node};
    auto suspended=SkillWorkflowCompiler(complete_runners).execute(approval_plan,approval_request.invocation,approval_request.session,approval_request.input);
    assert(!suspended.ok&&suspended.suspended&&suspended.error_code=="awaiting_approval"&&suspended.checkpoint_ref=="approval:pending");
    assert(suspended.resume_snapshot.at("waiting_node")=="approval");
    approval::ApprovalDecision approved;approved.metadata=stored.metadata;approved.approval_id="pending";approved.request_digest=approval::encode(stored).at("canonical_digest");approved.reviewer_id="reviewer";approved.decision=approval::Decision::Approved;approved.policy_revision="p1";approved.plan_digest="plan";approved.arguments_digest="sha256:args";
    assert(approval_store.decide(approved,0));
    assert(complete_runners->resolve(SkillRunnerKind::HumanApproval)->run(approval_request).ok);
    auto resumed=SkillWorkflowCompiler(complete_runners).execute(approval_plan,approval_request.invocation,approval_request.session,approval_request.input,{},suspended.resume_snapshot);
    assert(resumed.ok);
    auto tampered=suspended.resume_snapshot;tampered["node_outputs"]["forged"]={{"ok",true}};
    assert(SkillWorkflowCompiler(complete_runners).execute(approval_plan,approval_request.invocation,approval_request.session,approval_request.input,{},tampered).error_code=="resume_snapshot_integrity_failed");
    approval_request.input["arguments_digest"]="sha256:different";
    assert(complete_runners->resolve(SkillRunnerKind::HumanApproval)->run(approval_request).error_code=="stale_or_scope_mismatched_approval");
    std::filesystem::remove(approval_path);
    std::cout << "test_agent_template_runner_compiler: ok\n";
}
