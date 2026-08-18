#include <cassert>
#include <filesystem>

#include "agent/api/v1/session_run_api.hpp"
#include "agent/api/v1/event_multiplexer.hpp"

using namespace agent_framework;

identity::RuntimeSubject subject(std::string principal="owner") {
    identity::RuntimeSubject value;
    value.tenant_id="tenant";value.organization_id="org";
    value.principal_id=std::move(principal);value.project_id="project";
    value.workspace_id="workspace";value.session_id="request-session";
    value.conversation_id="request-conversation";value.agent_id="agent";
    value.authorization_revision=1;value.authenticated=true;
    return value;
}

session::ProductSession product() {
    session::ProductSession value;
    value.tenant_id="tenant";value.organization_id="org";value.project_id="project";
    value.workspace_id="workspace";value.session_id="session-1";
    value.conversation_id="conversation-1";value.owner_principal_id="owner";
    value.title="Production session";return value;
}

int main() {
    const auto root=std::filesystem::temp_directory_path()/"agent-session-run-api";
    std::filesystem::remove_all(root);std::filesystem::create_directories(root);
    session::SQLiteSessionCatalog catalog((root/"catalog.sqlite").string());
    session::SQLiteSessionRunSupervisor supervisor((root/"runs.sqlite").string());
    conversation::SQLiteConversationStore events((root/"events.sqlite").string());
    ui::SQLiteInteractionProjectionStore interactions((root/"interactions.sqlite").string());
    approval::SQLiteApprovalStore approvals((root/"approvals.sqlite").string());
    decision::SQLiteDecisionStore decisions((root/"decisions.sqlite").string());
    conversation::SQLiteTaskRegistry tasks((root/"tasks.sqlite").string());
    planning::InMemoryPlanStore plans;
    planning::InMemoryEvidenceStore evidence;
    api::v1::SessionRunApi api(catalog,supervisor,&events,&interactions,&approvals,&decisions,&tasks,
                               &plans,&evidence);

    auto owner=subject();auto created=api.create_session(owner,product());
    assert(created.status==201);
    assert(api.list_sessions(owner,{}).body.at("items").size()==1);
    assert(api.get_session(owner,"session-1").status==200);
    auto owner_capabilities=api.capabilities(owner,"session-1");assert(owner_capabilities.ok());
    auto find_action=[](const nlohmann::json& manifest,std::string_view id)->nlohmann::json {
        for(const auto& action:manifest.at("actions"))if(action.at("action_id")==id)return action;
        return nullptr;
    };
    assert(find_action(owner_capabilities.body,"session.transition").at("enabled")==true);
    assert(find_action(owner_capabilities.body,"approval.decide").at("enabled")==false);
    assert(find_action(owner_capabilities.body,"decision.answer").at("enabled")==true);

    auto outsider=subject("outsider");
    assert(api.get_session(outsider,"session-1").status==403);
    auto wrong_project=owner;wrong_project.project_id="other-project";
    assert(api.get_session(wrong_project,"session-1").status==403);
    assert(api.list_sessions(wrong_project,{}).body.at("items").empty());
    auto forged=owner;forged.tenant_id="other";
    assert(api.list_sessions(forged,{"tenant","owner"}).status==403);
    auto legacy=identity::legacy_local_subject("session-1","conversation-1");
    assert(api.get_session(legacy,"session-1").status==401);

    session::SessionMember viewer{"tenant","session-1","viewer",session::SessionMemberRole::Viewer};
    session::SessionMember contributor{"tenant","session-1","contributor",session::SessionMemberRole::Contributor};
    session::SessionMember op{"tenant","session-1","operator",session::SessionMemberRole::Operator};
    assert(catalog.put_member(viewer,0).ok&&catalog.put_member(contributor,0).ok&&catalog.put_member(op,0).ok);
    auto viewer_capabilities=api.capabilities(subject("viewer"),"session-1");
    assert(find_action(viewer_capabilities.body,"run.start").at("enabled")==false);
    assert(find_action(viewer_capabilities.body,"events.stream").at("enabled")==true);
    assert(api.rename_session(subject("viewer"),"session-1",1,"forbidden").status==403);
    auto renamed=api.rename_session(owner,"session-1",1,"Renamed production session");
    assert(renamed.ok()&&renamed.body.at("revision")==2);
    auto organized=api.organize_session(owner,"session-1",2,"Research",{"GNSS"},true);
    assert(organized.ok()&&organized.body.at("revision")==3);
    assert(api.put_session_member(subject("operator"),"session-1",0,"new-viewer",
                                  session::SessionMemberRole::Viewer).status==403);
    assert(api.put_session_member(owner,"session-1",0,"new-viewer",
                                  session::SessionMemberRole::Viewer).ok());

    session::SessionRunRequest run{"tenant","org","project","operator","provider",
        "session-1","run-1","start-1",{{"prompt","work"}},""};
    assert(api.enqueue_run(subject("viewer"),run).status==403);
    assert(api.enqueue_run(subject("operator"),run).status==202);
    assert(api.enqueue_run(subject("operator"),run).status==202);
    conversation::PersistentTask task;task.identity={"tenant","conversation-1"};
    task.task_id="task-1";task.root_turn_id="turn";task.current_turn_id="turn";
    task.current_run_id="run-1";
    conversation::TaskRequirementRevision requirement;requirement.turn_id="turn";
    requirement.content="work";
    conversation::TurnTaskLink task_link;task_link.turn_id="turn";task_link.run_id="run-1";
    assert(tasks.create(task,requirement,task_link).ok);
    planning::ExecutionPlan plan;plan.metadata.identity.tenant_id="tenant";
    plan.metadata.identity.organization_id="org";plan.metadata.identity.project_id="project";
    plan.metadata.identity.principal_id="conversation-1";plan.metadata.identity.task_id="task-1";
    plan.metadata.identity.plan_id="plan-1";assert(plans.create(plan));
    planning::EvidenceRecord evidence_record;evidence_record.evidence_id="evidence-1";
    evidence_record.origin_kind="repository";evidence_record.locator="repo:file.cpp";
    evidence_record.content_digest="sha256:evidence";evidence_record.trust_class="authoritative";
    contracts::ContractMetadata evidence_scope;evidence_scope.identity.tenant_id="tenant";
    evidence_scope.identity.principal_id="conversation-1";
    evidence_scope.identity.task_id="task-1";assert(evidence.append(evidence_scope,evidence_record));
    auto second_product=product();second_product.session_id="session-2";
    second_product.conversation_id="conversation-2";second_product.title="Isolated Session";
    assert(api.create_session(owner,second_product).status==201);
    conversation::PersistentTask second_task=task;second_task.identity={"tenant","conversation-2"};
    second_task.root_turn_id="turn-2";second_task.current_turn_id="turn-2";
    second_task.current_run_id="run-2";
    auto second_run=run;second_run.session_id="session-2";second_run.run_id="run-2";
    second_run.command_id="start-2";assert(supervisor.enqueue(second_run).ok);
    conversation::TaskRequirementRevision second_requirement;second_requirement.turn_id="turn-2";
    second_requirement.content="isolated work";
    conversation::TurnTaskLink second_link;second_link.turn_id="turn-2";second_link.run_id="run-2";
    assert(tasks.create(second_task,second_requirement,second_link).ok);
    auto second_plan=plan;second_plan.metadata.identity.principal_id="conversation-2";
    second_plan.planning_view_digest="sha256:session-2-view";assert(plans.create(second_plan));
    auto second_evidence=evidence_record;second_evidence.locator="repo:session-2.cpp";
    second_evidence.content_digest="sha256:session-2-evidence";
    auto second_evidence_scope=evidence_scope;
    second_evidence_scope.identity.principal_id="conversation-2";
    assert(evidence.append(second_evidence_scope,second_evidence));

    session::SessionRunCommand comment{"tenant","session-1","run-1","comment-1",
        session::SessionCommandKind::Comment,{{"text","note"}}};
    comment.expected_run_revision=1;
    assert(api.enqueue_command(subject("contributor"),comment).status==202);
    session::SessionRunCommand cancel{"tenant","session-1","run-1","cancel-1",
        session::SessionCommandKind::Cancel,{}};
    cancel.expected_run_revision=1;
    assert(api.enqueue_command(subject("contributor"),cancel).status==403);
    assert(api.enqueue_command(subject("operator"),cancel).status==202);
    assert(api.get_run(subject("viewer"),"run-1").status==200);
    assert(api.get_run(outsider,"run-1").status==403);

    auto append=[&](std::uint64_t sequence,conversation::EventVisibility visibility) {
        conversation::RuntimeEventEnvelope event;event.event_id="event-"+std::to_string(sequence);
        event.tenant_id="tenant";event.conversation_id="conversation-1";event.turn_id="turn";
        event.run_id="run-1";event.sequence=sequence;event.durability=conversation::EventDurability::Durable;
        event.visibility=visibility;event.event_type=sequence==2?"task_semantics_decided":"test";
        event.timestamp="now";if(sequence==2)event.payload={{"task_id","task-1"},{"work_shape","bounded_task"}};
        assert(events.append_event(event,nullptr));
    };
    append(1,conversation::EventVisibility::Internal);
    append(2,conversation::EventVisibility::User);
    append(3,conversation::EventVisibility::Operations);
    auto viewer_events=api.replay_events(subject("viewer"),"session-1",0,10);
    assert(viewer_events.ok()&&viewer_events.body.at("items").size()==1&&viewer_events.body.at("next_cursor")==3);
    auto operator_events=api.replay_events(subject("operator"),"session-1",0,10);
    assert(operator_events.body.at("items").size()==2);
    auto first_scan=api.replay_events(subject("viewer"),"session-1",0,1);
    assert(first_scan.body.at("items").empty()&&first_scan.body.at("next_cursor")==1&&first_scan.body.at("has_more")==true);
    assert(api.replay_events(subject("viewer"),"session-1",4,10).status==409);

    ui::InteractionRef ref;ref.tenant_id="tenant";ref.conversation_id="conversation-1";
    ref.turn_id="turn";ref.task_id="task-1";ref.run_id="run-1";ref.artifact_id="sha256:artifact";
    ui::InteractionSourceRevision source{"assurance","artifact",1,"sha256:source"};
    ui::InteractionNode artifact;artifact.node_id="artifact:sha256:artifact";
    artifact.kind=ui::InteractionNodeKind::Artifact;artifact.ref=ref;artifact.label="Report";
    artifact.summary="Verified report";artifact.display={{"media_type","text/markdown"}};
    artifact.state=ui::InteractionObjectState::Passed;artifact.visibility=ui::InteractionVisibility::User;
    artifact.source=source;artifact.updated_at="now";
    ui::UiInteractionEvent projection_event;projection_event.event_id="projection-1";
    projection_event.tenant_id="tenant";projection_event.conversation_id="conversation-1";
    projection_event.sequence=1;projection_event.event_type="artifact.updated";
    projection_event.visibility=ui::InteractionVisibility::User;projection_event.primary_ref=ref;
    projection_event.display={{"label","Report"}};projection_event.navigation_target={"artifact","sha256:artifact",1};
    projection_event.source=source;projection_event.timestamp="now";
    assert(interactions.commit({projection_event,{artifact},{}},0).ok());
    assert(api.get_artifact(subject("viewer"),"session-1","sha256:artifact").status==200);
    assert(api.get_artifact(outsider,"session-1","sha256:artifact").status==403);
    auto interaction_view=api.get_interactions(subject("viewer"),"session-1");
    assert(interaction_view.ok()&&interaction_view.body.at("head_sequence")==1&&
           interaction_view.body.at("runtime_event_head")==3&&interaction_view.body.at("stale")==true);
    assert(api.get_interactions(subject("viewer"),"session-1",ui::InteractionVisibility::Audit).status==403);
    auto task_aggregate=api.get_task(subject("viewer"),"session-1","task-1");
    assert(task_aggregate.ok()&&task_aggregate.body.at("schema")=="agent.task_aggregate/v1"&&
           task_aggregate.body.at("requirements").size()==1);
    assert(api.get_task(outsider,"session-1","task-1").status==403);
    const auto semantics=api.get_task_semantics(subject("viewer"),"session-1","task-1");
    assert(semantics.ok()&&semantics.body.at("schema")=="agent.task_semantics_projection/v1"&&
           semantics.body.at("items").size()==1);
    const auto plan_view=api.get_plan(subject("viewer"),"session-1","task-1","plan-1");
    assert(plan_view.ok()&&plan_view.body.at("kind")=="agent.execution_plan/v1");
    const auto observations=api.get_observations(subject("viewer"),"session-1","task-1");
    assert(observations.ok()&&observations.body.at("schema")=="agent.task_observation_projection/v1"&&
           observations.body.at("items").size()==1);
    const auto evidence_view=api.get_evidence(subject("viewer"),"session-1","task-1","evidence-1");
    assert(evidence_view.ok()&&evidence_view.body.at("kind")=="agent.evidence_bundle/v1"&&
           evidence_view.body.at("payload").at("records").size()==1);
    const auto isolated_plan=api.get_plan(owner,"session-2","task-1","plan-1");
    assert(isolated_plan.ok()&&isolated_plan.body.at("payload").at("planning_view_digest")==
           "sha256:session-2-view");
    const auto isolated_evidence=api.get_evidence(owner,"session-2","task-1","evidence-1");
    assert(isolated_evidence.ok()&&isolated_evidence.body.at("payload").at("records").at(0)
           .at("content_digest")=="sha256:session-2-evidence");
    auto snapshot=api.get_execution_snapshot(subject("viewer"),"session-1","task-1","run-1");
    assert(snapshot.ok()&&snapshot.body.at("task_revision")==1&&
           snapshot.body.at("run_revision")==1&&snapshot.body.at("projection_revision")==1);
    assert(!snapshot.body.at("snapshot_digest").get<std::string>().empty());

    approval::ApprovalRequest approval_request;approval_request.metadata.identity.tenant_id="tenant";
    approval_request.metadata.identity.organization_id="org";approval_request.metadata.identity.project_id="project";
    approval_request.metadata.identity.task_id="task-1";approval_request.metadata.identity.run_id="run-1";
    approval_request.approval_id="approval-1";approval_request.request_kind="tool";
    approval_request.requester_id="operator";approval_request.scope="tool:write";
    approval_request.reason="side effect";approval_request.policy_revision="policy-v1";
    approval_request.created_at="now";approval_request.expires_at="later";
    assert(approvals.put_request(approval_request));
    assert(api.get_approval(subject("viewer"),"session-1","approval-1").status==403);
    assert(api.get_approval(subject("contributor"),"session-1","approval-1").status==200);
    decision::DecisionRequest decision;decision.subject.tenant_id="tenant";
    decision.subject.session_id="session-1";decision.subject.conversation_id="conversation-1";
    decision.subject.task_id="task-1";decision.subject.run_id="run-1";decision.subject.turn_id="turn";
    decision.decision_id="decision-1";decision.question="Choose task scope";
    decision.options={{"bounded","Bounded","One bounded task",{{"work_shape","bounded_task"}}},
                      {"long","Long","Comprehensive task",{{"work_shape","long_running_task"}}}};
    decision.origin_digest="sha256:decision";decision.expires_at_ms=9999999999999ULL;
    decision.created_at="1";decision.updated_at="1";assert(decisions.create(decision).ok);
    assert(api.get_decision(subject("viewer"),"session-1","decision-1").status==200);
    assert(api.answer_decision(subject("viewer"),"session-1","decision-1",1,"bounded",1).status==403);
    assert(api.answer_decision(subject("contributor"),"session-1","decision-1",1,"bounded",1).status==200);
    auto replayed=api.answer_decision(subject("contributor"),"session-1","decision-1",1,"bounded",1);
    assert(replayed.status==200&&replayed.body.at("replayed")==true);
    assert(api.answer_decision(subject("contributor"),"session-1","decision-1",1,"long",1).status==409);
    api::v1::AuthorizedEventMultiplexer multiplexer(api,2);
    auto multiplexed=multiplexer.poll(subject("viewer"),{{"session-1",0}},10);
    assert(multiplexed.ok&&multiplexed.cursors.at(0).after==3&&multiplexed.frames.size()==1);
    assert(!multiplexer.poll(subject("viewer"),{{"session-1",0},{"session-1",1}},10).ok);
    assert(!multiplexer.poll(outsider,{{"session-1",0}},10).ok);

    auto archived=api.transition_session(owner,"session-1",3,session::ProductSessionState::Archived);
    assert(archived.ok()&&archived.body.at("revision")==4);
    assert(api.transition_session(owner,"session-1",3,session::ProductSessionState::Active).status==409);
    std::filesystem::remove_all(root);
}
