#include <agent/agent_template/compiler.hpp>
#include <agent/agent_template/registry.hpp>
#include <agent/agent_template/runner.hpp>
#include <agent/agent_template/session.hpp>
#include <agent/approval/store.hpp>
#include <agent/harness/task_closure.hpp>
#include <agent/toolbus/fs_tools.hpp>
#include <agent/toolbus/process_tools.hpp>
#include <agent/toolbus/web_tools.hpp>

#include <httplib.hpp>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

using namespace agent_framework;
using namespace agent_framework::agent_template;
namespace fs = std::filesystem;

namespace {
contracts::ContractMetadata metadata() {
    contracts::ContractMetadata m;m.identity.tenant_id="at8-tenant";m.identity.task_id="at8-task";return m;
}
SkillPlanNode tool_node(std::string id,std::string tool,json arguments,EffectClass effect) {
    SkillPlanNode n;n.node_id=std::move(id);n.runner=SkillRunnerKind::LocalCapability;
    n.resolved_skill_id="portable-build";n.requested_permissions.tools={tool};n.effect=effect;
    n.input_mapping={{"tool",{{"value",tool}}},{"arguments",{{"value",std::move(arguments)}}}};
    return n;
}
void write_skill(const fs::path& root) {
    fs::create_directories(root/".claude/skills/portable-build");
    std::ofstream(root/".claude/skills/portable-build/SKILL.md") << R"(---
api-version: agent.taskflow/v1
kind: Skill
name: portable-build
version: 1.0.0
description: Portable build and evidence workflow used without source rewriting.
permissions:
  tools: [Write, Edit, Bash, CMake, Make, Read, WebFetch]
---
Use canonical portable tools to edit, build, inspect and collect evidence.)";
}
}

int main() {
    const auto root=fs::temp_directory_path()/"agent-at8-portable-e2e";std::error_code ec;fs::remove_all(root,ec);fs::create_directories(root);
    write_skill(root);
    ::setenv("AGENT_FS_ROOT",root.c_str(),1);::setenv("AGENT_WEB_ENABLE","1",1);
    ::setenv("AGENT_WEB_ALLOW_HTTP","1",1);::setenv("AGENT_WEB_TEST_ALLOW_LOOPBACK","1",1);
    ::setenv("AGENT_TOOL_ALLOWLIST","",1);

    httplib::Server server;server.Get("/evidence",[](const httplib::Request&,httplib::Response&r){r.set_content("<main>AT8 portable evidence</main>","text/html");});
    const int port=server.bind_to_any_port("127.0.0.1");assert(port>0);std::thread server_thread([&]{server.listen_after_bind();});
    for(int i=0;i<50&&!server.is_running();++i)std::this_thread::sleep_for(std::chrono::milliseconds(20));assert(server.is_running());

    auto bus=std::make_shared<ToolBus>();register_builtin_fs_tools_if_configured(*bus);register_builtin_process_tools_if_configured(*bus);register_builtin_web_tools_if_configured(*bus);
    for(const auto* name:{"Write","Edit","Bash","CMake","Make","Read","WebFetch"})assert(bus->get_tool_info(name));
    auto skills=std::make_shared<SkillRegistry>(root/".claude/skills");skills->scan_or_reload();auto snapshot=skills->snapshot();assert(snapshot.valid());
    const auto& entries=snapshot.entries();const auto skill=std::find_if(entries.begin(),entries.end(),[](const auto&s){return s.id=="portable-build";});assert(skill!=entries.end()&&skill->manifest);

    SkillCollaborationPlan plan;plan.metadata=metadata();plan.plan_id="at8-portable-e2e";plan.budget.max_parallelism=1;
    plan.nodes={
      tool_node("write-cmake","Write",{{"path","CMakeLists.txt"},{"content","cmake_minimum_required(VERSION 3.16)\nproject(at8 LANGUAGES CXX)\nadd_executable(at8 main.cpp)\n"}},EffectClass::Write),
      tool_node("write-source","Write",{{"path","main.cpp"},{"content","#include <iostream>\nint main(){std::cout << \"OLD\";}\n"}},EffectClass::Write),
      tool_node("edit-source","Edit",{{"path","main.cpp"},{"old_string","OLD"},{"new_string","PORTABLE"},{"dry_run",false},{"confirm_write",true}},EffectClass::Write),
      tool_node("side-effect","Bash",{{"command","printf x >> side-effects.log"}},EffectClass::Write),
      tool_node("configure","CMake",{{"args",json::array({"-S","/workspace","-B","/workspace/cmake-build"})}},EffectClass::Write),
      tool_node("build","Make",{{"args",json::array({"-C","/workspace/cmake-build"})}},EffectClass::Write)
    };
    SkillPlanNode approval;approval.node_id="approval";approval.runner=SkillRunnerKind::HumanApproval;
    approval.input_mapping={{"approval_id",{{"value","at8-approval"}}},{"arguments_digest",{{"value","sha256:at8-arguments"}}},{"policy_revision",{{"value","at8-policy-v1"}}}};
    plan.nodes.push_back(approval);
    plan.nodes.push_back(tool_node("read","Read",{{"path","main.cpp"}},EffectClass::ReadOnly));
    plan.nodes.push_back(tool_node("fetch","WebFetch",{{"url","http://127.0.0.1:"+std::to_string(port)+"/evidence"},{"extract_mode","text"}},EffectClass::ReadOnly));
    for(auto& n:plan.nodes){n.resolved_skill_id="portable-build";n.resolved_skill_version=skill->version;n.resolved_skill_digest=skill->package_digest;}
    for(std::size_t i=1;i<plan.nodes.size();++i)plan.edges.push_back({plan.nodes[i-1].node_id,plan.nodes[i].node_id,{}});plan.output_assembly={{"from","fetch"}};

    PermissionEnvelope permissions;permissions.tools={"Write","Edit","Bash","CMake","Make","Read","WebFetch"};
    SessionBuildRequest session_request{metadata(),"at8-session",plan,snapshot,permissions,{}, {},"at8-deployment"};
    auto built=ActiveSkillSessionBuilder().build(session_request);assert(built.session&&built.session->skills.size()==1);
    AgentTemplateInvocation invocation;invocation.metadata=metadata();invocation.invocation_id="at8-invocation";invocation.template_ref={"at8-portable",1,"sha256:at8-template"};invocation.plan_digest=encode(plan).at("canonical_digest");invocation.skill_session_digest=encode(*built.session).at("canonical_digest");invocation.model_profiles_digest="sha256:at8-model";invocation.capability_snapshot_digest=built.session->capability_snapshot_digest;invocation.deployment_generation="at8-deployment";

    const auto registry_path=(root/"registry.sqlite3").string(),approval_path=(root/"approval.sqlite3").string();
    auto registry=std::make_shared<SQLiteAgentTemplateRegistry>(registry_path);assert(registry->create_invocation(invocation).ok());
    approval::SQLiteApprovalStore approval_store(approval_path);approval::ApprovalRequest request;request.metadata=metadata();request.approval_id="at8-approval";request.requester_id="at8-agent";request.policy_revision="at8-policy-v1";request.plan_digest=invocation.plan_digest;request.arguments_digest="sha256:at8-arguments";assert(approval_store.put_request(request));
    auto runners=build_production_runners({bus,{},{},&approval_store});auto first=SkillWorkflowCompiler(runners).execute(plan,invocation,*built.session,json::object());
    assert(first.suspended&&first.error_code=="awaiting_approval");assert(fs::file_size(root/"side-effects.log")==1);assert(fs::exists(root/"cmake-build/at8"));
    StoredExecutionCheckpoint checkpoint{metadata().identity.tenant_id,invocation.invocation_id,0,first.resume_snapshot,"",""};assert(registry->save_execution_checkpoint(checkpoint,0).ok());

    approval::ApprovalDecision decision;decision.metadata=request.metadata;decision.approval_id=request.approval_id;decision.request_digest=approval::encode(request).at("canonical_digest");decision.reviewer_id="at8-reviewer";decision.decision=approval::Decision::Approved;decision.policy_revision=request.policy_revision;decision.plan_digest=request.plan_digest;decision.arguments_digest=request.arguments_digest;assert(approval_store.decide(decision,0));
    registry.reset();runners.reset();
    auto restarted_registry=std::make_shared<SQLiteAgentTemplateRegistry>(registry_path);approval::SQLiteApprovalStore restarted_approval(approval_path);auto restarted_runners=build_production_runners({bus,{},{},&restarted_approval});
    auto loaded=restarted_registry->load_execution_checkpoint(metadata().identity.tenant_id,invocation.invocation_id);assert(loaded);
    auto resumed=SkillWorkflowCompiler(restarted_runners).execute(plan,invocation,*built.session,json::object(),{},loaded->snapshot);
    assert(resumed.ok);assert(fs::file_size(root/"side-effects.log")==1);assert(resumed.output.dump().find("AT8 portable evidence")!=std::string::npos);

    harness::TaskClosureContract contract;contract.metadata=metadata();contract.contract_id="at8-closure";contract.revision="1";contract.task_class="portable-e2e";contract.deliverables={"binary"};contract.mandatory_criteria={"portable-build","restart-safe","evidence-collected"};contract.verification_methods={{"portable-build",{"binary"}},{"restart-safe",{"side-effect-count"}},{"evidence-collected",{"WebFetch"}}};
    harness::ClosureFacts facts;facts.checkpoint.state=harness::HarnessState::Completed;facts.checkpoint.judge_required=false;
    facts.checkpoint.pins={"sha256:intake",invocation.plan_digest,"sha256:acceptance","memory-at8","sha256:memory","sha256:profile","sha256:prompt","at8-approval","sha256:artifact","sha256:report","","sha256:operations"};
    for(const auto stage:{harness::HarnessStage::Intake,harness::HarnessStage::Cognition,harness::HarnessStage::PlanApproval,harness::HarnessStage::Execution,harness::HarnessStage::MemoryUpdate,harness::HarnessStage::Assurance,harness::HarnessStage::Operations})facts.checkpoint.stage_records.push_back({stage,1,harness::StageOutcome::Succeeded});
    facts.strong_evidence_refs={contracts::embedded_digest(resumed.output).value_or("sha256:evidence")};facts.artifact_refs={"file:cmake-build/at8"};facts.last_progress_revision=1;
    for(const auto&id:contract.mandatory_criteria)facts.criterion_verdicts.push_back({id,"pass",facts.strong_evidence_refs,facts.artifact_refs,contract.verification_methods.at(id).front(),"at8-verifier","sha256:at8-report",1});
    const auto closure=harness::TaskClosureController().evaluate(contract,facts);assert(closure.state==harness::TaskTerminalState::CompletedVerified);
    server.stop();server_thread.join();fs::remove_all(root,ec);std::cout<<"test_at8_portable_e2e: ok\n";
}
