#include <filesystem>
#include <iostream>
#include <chrono>
#include <thread>
#include <httplib.hpp>
#include "agent/api/v1/http_routes.hpp"
#include "agent/session/run_worker.hpp"
#include "agent/ui/runtime_event_projector.hpp"

#ifndef AGENT_WORKBENCH_DIST
#define AGENT_WORKBENCH_DIST "."
#endif

using namespace agent_framework;
int main(int argc,char** argv) {
    const int port=argc>1?std::stoi(argv[1]):4174;
    const auto root=std::filesystem::temp_directory_path()/"agent-workbench-runtime";
    std::error_code ec;std::filesystem::remove_all(root,ec);std::filesystem::create_directories(root);
    session::SQLiteSessionCatalog catalog((root/"catalog.sqlite").string());
    session::SQLiteSessionRunSupervisor supervisor((root/"runs.sqlite").string());
    conversation::SQLiteConversationStore events((root/"events.sqlite").string());
    ui::SQLiteInteractionProjectionStore interactions((root/"interactions.sqlite").string());
    approval::SQLiteApprovalStore approvals((root/"approvals.sqlite").string());
    decision::SQLiteDecisionStore decisions((root/"decisions.sqlite").string());
    conversation::SQLiteTaskRegistry tasks((root/"tasks.sqlite").string());
    session::ProductSession product;product.tenant_id="local";product.organization_id="local";
    product.project_id="local";product.workspace_id="local";product.session_id="session-orbital";
    product.conversation_id="conversation-orbital";product.owner_principal_id="local-user";
    product.title="Orbital analysis and runtime closure";product.folder="Production certification";
    product.tags={"GNSS","Phase 4"};catalog.create(product);
    identity::RuntimeSubject subject;subject.tenant_id="local";subject.organization_id="local";
    subject.project_id="local";subject.workspace_id="local";subject.principal_id="local-user";
    subject.session_id=product.session_id;subject.conversation_id=product.conversation_id;
    subject.agent_id="workbench";subject.authorization_revision=1;subject.authenticated=true;
    session::SessionRunRequest seeded_run{"local","local","local","local-user","workbench-provider",
        product.session_id,"run-orbital","start-run-orbital",{{"input","Verify orbital workflow"}},
        "2026-08-18T09:00:00Z"};
    supervisor.enqueue(seeded_run);
    conversation::PersistentTask task;task.identity={"local",product.conversation_id};
    task.task_id="task-orbital";task.root_turn_id="turn-orbital";
    task.current_turn_id="turn-orbital";task.current_run_id="run-orbital";
    conversation::TaskRequirementRevision requirement;requirement.turn_id="turn-orbital";
    requirement.content="Verify orbital workflow with professional assurance";
    conversation::TurnTaskLink link;link.turn_id="turn-orbital";link.run_id="run-orbital";
    tasks.create(task,requirement,link);
    decision::DecisionRequest choice;choice.subject=subject;choice.subject.task_id="task-orbital";
    choice.subject.run_id="run-orbital";choice.subject.turn_id="turn-orbital";
    choice.decision_id="scope-decision";choice.question="How deeply should the orbital workflow be verified?";
    choice.options={{"functional","Functional verification","Run module and integration checks",{{"work_shape","bounded_task"},{"assurance_tier","functional"}}},
                    {"professional","Professional assurance","Execute domain evidence, judge and reverification",{{"work_shape","long_running_task"},{"assurance_tier","professional"}}}};
    choice.recommended_option_id="professional";choice.origin_digest="sha256:workbench-decision";
    choice.expires_at_ms=9999999999999ULL;choice.created_at="2026-08-18T09:00:00Z";choice.updated_at=choice.created_at;
    decisions.create(choice);
    auto emit=[&](std::uint64_t sequence,std::string type,nlohmann::json payload) {
        conversation::RuntimeEventEnvelope event;event.event_id="workbench-"+std::to_string(sequence);
        event.tenant_id="local";event.conversation_id=product.conversation_id;event.turn_id="turn-orbital";
        event.run_id="run-orbital";event.sequence=sequence;event.durability=conversation::EventDurability::Durable;
        event.visibility=conversation::EventVisibility::User;event.event_type=std::move(type);
        event.timestamp="2026-08-18T09:0"+std::to_string(sequence)+":00Z";
        payload["task_id"]="task-orbital";event.payload=std::move(payload);
        if(!events.append_event(event,nullptr))throw std::runtime_error("seed event failed");};
    emit(1,"task_semantics_decided",{{"state","passed"},{"summary","Long-running read-only analysis · professional assurance"},{"work_shape","long_running_task"},{"effect_class","read_only"}});
    emit(2,"decision_requested",{{"state","waiting"},{"summary","Verification depth needs confirmation"},{"decision_id","scope-decision"}});
    emit(3,"planning_required",{{"state","running"},{"summary","Comprehensive plan required by assurance policy"},{"planning_depth","comprehensive"}});
    emit(4,"memory_view_assembled",{{"state","passed"},{"summary","Five-layer governed context assembled"},{"records",18}});
    emit(5,"tool_observation_committed",{{"state","running"},{"summary","Repository inventory synchronized"},{"tool","fs_search"}});
    ui::RuntimeEventInteractionProjector projector(events,interactions);
    if(auto projected=projector.synchronize({"local",product.conversation_id});!projected.ok)
        throw std::runtime_error("initial interaction projection failed: "+projected.error);
    api::v1::SessionRunApi api(catalog,supervisor,&events,&interactions,&approvals,&decisions,&tasks);
    session::SessionRunWorker worker(supervisor,"workbench-worker",2000,
        [&](session::WorkerExecutionContext& context) {
            for(const auto& command:context.commands)
                if(command.kind==session::SessionCommandKind::Cancel)
                    return session::WorkerExecutionResult{session::WorkerDisposition::Cancelled,"cancelled_by_command"};
            if(auto pending=decisions.pending(subject.tenant_id,product.session_id,
                                              product.conversation_id);
               pending&&pending->subject.run_id==context.run.request.run_id)
                return session::WorkerExecutionResult{session::WorkerDisposition::AwaitingInput,"decision_pending"};
            const auto sequence=events.last_event_sequence({"local",product.conversation_id})+1;
            emit(sequence,"run_completed",{{"state","completed"},
                {"summary","Run completed by leased production worker"},
                {"run_revision",context.run.revision},{"lease_epoch",context.run.lease_epoch}});
            if(auto projected=projector.synchronize({"local",product.conversation_id});!projected.ok)
                return session::WorkerExecutionResult{session::WorkerDisposition::Failed,projected.error};
            return session::WorkerExecutionResult{session::WorkerDisposition::Completed,{}};
        });
    std::jthread worker_thread([&](std::stop_token stop) {
        while(!stop.stop_requested()) {
            const auto now=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            worker.tick(now);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    httplib::Server server;api::v1::register_session_run_routes(server,api,[subject](const auto&){return subject;});
    const std::string dist=AGENT_WORKBENCH_DIST;
    if(!server.set_mount_point("/",dist.c_str())){std::cerr<<"workbench dist missing: "<<dist<<'\n';return 2;}
    std::cout<<"workbench runtime http://127.0.0.1:"<<port<<std::endl;
    return server.listen("127.0.0.1",port)?0:3;
}
