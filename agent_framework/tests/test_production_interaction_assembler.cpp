#include <cassert>
#include <filesystem>
#include <set>

#include "agent/internal/platform_io.hpp"
#include "agent/ui/production_interaction_assembler.hpp"

using namespace agent_framework;

int main(){
    const auto root=std::filesystem::temp_directory_path()/("interaction-production-"+std::to_string(internal::current_process_id()));
    std::error_code ec;std::filesystem::remove_all(root,ec);std::filesystem::create_directories(root);
    conversation::SQLiteConversationStore conversations((root/"conversation.sqlite3").string());
    run::SQLiteRunStore runs((root/"run.sqlite3").string(),{3000,false});
    planning::InMemoryPlanStore plans;InMemorySessionStore sessions;
    decision::SQLiteDecisionStore decisions((root/"decision.sqlite3").string());
    contracts::ContractIdentity identity;identity.tenant_id="tenant-a";identity.task_id="task-a";identity.run_id="run-a";identity.plan_id="plan-a";
    conversation::ConversationMessage message;message.identity={"tenant-a","conversation-a"};message.message_id="message-a";message.turn_id="turn-a";message.role="user";message.content="Verify and repair the production workflow.";message.created_at="2026-08-15T00:00:00Z";message.sequence=1;
    assert(conversations.append_message(message,nullptr));
    run::RunCheckpoint checkpoint;checkpoint.metadata.identity=identity;checkpoint.state=run::RunState::Running;checkpoint.graph_revision="graph-v1";checkpoint.plan_digest="sha256:plan";checkpoint.created_at="2026-08-15T00:00:00Z";assert(runs.create(checkpoint));
    planning::ExecutionPlan plan;plan.metadata.identity=identity;plan.plan_revision=1;planning::PlanNode work;work.node_id="work";work.objective="Implement verified closure";work.risk_level="medium";plan.nodes.push_back(work);plan.critical_path={"work"};assert(plans.create(plan));
    auto session=sessions.load_or_create("session-a");session.checkpoint_id="cp-a";session.child_tasks={{"planner","local",1,"running",nlohmann::json::object()},{"verifier","remote",1,"completed",nlohmann::json::object()}};session.tool_commits={{"tool-a",1,"completed","sha256:result"}};assert(sessions.commit(session,0).status==SessionCommitStatus::Committed);
    decision::DecisionRequest choice;choice.subject.tenant_id="tenant-a";choice.subject.session_id="session-a";
    choice.subject.conversation_id="conversation-a";choice.subject.task_id="task-a";choice.subject.run_id="run-a";
    choice.subject.turn_id="turn-a";choice.decision_id="decision-a";choice.question="Which scope should be used?";
    choice.options={{"bounded","Bounded task","Apply one verified change",{{"work_shape","bounded_task"}}},
                    {"long","Long task","Perform comprehensive work",{{"work_shape","long_running_task"}}}};
    choice.origin_digest="sha256:decision";choice.expires_at_ms=9999999999999ULL;choice.created_at="1";choice.updated_at="1";
    assert(decisions.create(choice).ok);
    ui::ProductionInteractionQuery query;query.identity=identity;query.conversation_id="conversation-a";query.turn_id="turn-a";query.message_id="message-a";query.session_id="session-a";query.now="2026-08-15T00:01:00Z";
    ui::ProductionInteractionStores stores{&conversations,&runs,&plans,&sessions};stores.decisions=&decisions;
    auto result=ui::assemble_production_interactions(stores,query);assert(result);
    std::set<ui::InteractionNodeKind> kinds;for(const auto& n:result.snapshot->nodes)kinds.insert(n.kind);
    assert(kinds.count(ui::InteractionNodeKind::Message));assert(kinds.count(ui::InteractionNodeKind::Plan));assert(kinds.count(ui::InteractionNodeKind::Agent));assert(kinds.count(ui::InteractionNodeKind::ToolInvocation));
    assert(kinds.count(ui::InteractionNodeKind::Understanding));assert(kinds.count(ui::InteractionNodeKind::Decision));
    assert(std::count_if(result.snapshot->edges.begin(),result.snapshot->edges.end(),[](const auto&e){return e.kind==ui::InteractionEdgeKind::ParentOf;})==1);
    auto encoded=ui::encode(*result.snapshot);std::vector<contracts::ContractIssue> issues;assert(ui::decode_interaction_snapshot(encoded,&issues));assert(issues.empty());
    auto bad=query;bad.identity.tenant_id="tenant-b";auto rejected=ui::assemble_production_interactions(stores,bad);assert(!rejected&&rejected.error==ui::InteractionAssemblerError::MissingRequiredObject);
    ui::ProductionInteractionStores missing;auto absent=ui::assemble_production_interactions(missing,query);assert(!absent&&absent.error==ui::InteractionAssemblerError::MissingRequiredStore);
    std::filesystem::remove_all(root,ec);
}
