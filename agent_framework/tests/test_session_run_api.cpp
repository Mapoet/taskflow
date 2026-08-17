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
    api::v1::SessionRunApi api(catalog,supervisor,&events,&interactions,&approvals);

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

    session::SessionRunRequest run{"tenant","org","project","operator","provider",
        "session-1","run-1","start-1",{{"prompt","work"}},""};
    assert(api.enqueue_run(subject("viewer"),run).status==403);
    assert(api.enqueue_run(subject("operator"),run).status==202);
    assert(api.enqueue_run(subject("operator"),run).status==202);

    session::SessionRunCommand comment{"tenant","session-1","run-1","comment-1",
        session::SessionCommandKind::Comment,{{"text","note"}}};
    assert(api.enqueue_command(subject("contributor"),comment).status==202);
    session::SessionRunCommand cancel{"tenant","session-1","run-1","cancel-1",
        session::SessionCommandKind::Cancel,{}};
    assert(api.enqueue_command(subject("contributor"),cancel).status==403);
    assert(api.enqueue_command(subject("operator"),cancel).status==202);
    assert(api.get_run(subject("viewer"),"run-1").status==200);
    assert(api.get_run(outsider,"run-1").status==403);

    auto append=[&](std::uint64_t sequence,conversation::EventVisibility visibility) {
        conversation::RuntimeEventEnvelope event;event.event_id="event-"+std::to_string(sequence);
        event.tenant_id="tenant";event.conversation_id="conversation-1";event.turn_id="turn";
        event.run_id="run-1";event.sequence=sequence;event.durability=conversation::EventDurability::Durable;
        event.visibility=visibility;event.event_type="test";event.timestamp="now";
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
    ref.turn_id="turn";ref.run_id="run-1";ref.artifact_id="sha256:artifact";
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
    api::v1::AuthorizedEventMultiplexer multiplexer(api,2);
    auto multiplexed=multiplexer.poll(subject("viewer"),{{"session-1",0}},10);
    assert(multiplexed.ok&&multiplexed.cursors.at(0).after==3&&multiplexed.frames.size()==1);
    assert(!multiplexer.poll(subject("viewer"),{{"session-1",0},{"session-1",1}},10).ok);
    assert(!multiplexer.poll(outsider,{{"session-1",0}},10).ok);

    auto archived=api.transition_session(owner,"session-1",1,session::ProductSessionState::Archived);
    assert(archived.ok()&&archived.body.at("revision")==2);
    assert(api.transition_session(owner,"session-1",1,session::ProductSessionState::Active).status==409);
    std::filesystem::remove_all(root);
}
