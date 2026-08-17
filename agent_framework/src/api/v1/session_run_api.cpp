#include "agent/api/v1/session_run_api.hpp"

namespace agent_framework::api::v1 {
namespace {
using nlohmann::json;

bool production_identity_valid(const identity::RuntimeSubject& subject) {
    return identity::validate(subject, identity::SubjectBoundary::Production).empty();
}

int rank(session::SessionMemberRole role) {
    switch(role) {
        case session::SessionMemberRole::Viewer: return 0;
        case session::SessionMemberRole::Contributor: return 1;
        case session::SessionMemberRole::Operator: return 2;
        case session::SessionMemberRole::Owner: return 3;
    }
    return -1;
}

json encode(const session::ProductSession& value) {
    return {{"tenant_id",value.tenant_id},{"organization_id",value.organization_id},
        {"project_id",value.project_id},{"workspace_id",value.workspace_id},
        {"session_id",value.session_id},{"conversation_id",value.conversation_id},
        {"owner_principal_id",value.owner_principal_id},{"title",value.title},
        {"folder",value.folder},{"tags",value.tags},{"pinned",value.pinned},
        {"state",session::name(value.state)},{"revision",value.revision},
        {"sequence",value.sequence},{"created_at",value.created_at},
        {"updated_at",value.updated_at}};
}

json encode(const session::SupervisedRun& value) {
    return {{"tenant_id",value.request.tenant_id},{"organization_id",value.request.organization_id},
        {"project_id",value.request.project_id},{"principal_id",value.request.principal_id},
        {"provider_id",value.request.provider_id},{"session_id",value.request.session_id},
        {"run_id",value.request.run_id},{"command_id",value.request.command_id},
        {"payload",value.request.payload},{"state",session::name(value.state)},
        {"revision",value.revision},{"lease_epoch",value.lease_epoch},
        {"lease_expires_at_ms",value.lease_expires_at_ms},
        {"created_at",value.request.created_at},{"updated_at",value.updated_at}};
}

ApiResult mutation(const session::SessionMutationResult& result) {
    if(result.ok) return {200,{{"ok",true},{"revision",result.revision}}};
    const int status=result.error=="revision_conflict"?409:
        result.error=="resource_not_found"?404:422;
    return {status,{{"error",result.error},{"revision",result.revision}}};
}

ApiResult run_mutation(const session::RunSupervisorResult& result, int accepted=202) {
    if(result.ok) return {accepted,{{"accepted",true},{"revision",result.revision},
                                    {"lease_epoch",result.lease_epoch}}};
    const int status=result.error.find("conflict")!=std::string::npos?409:
        result.error.find("not_found")!=std::string::npos?404:422;
    return {status,{{"error",result.error},{"revision",result.revision}}};
}
} // namespace

ApiResult SessionRunApi::denied(std::string code,int status) {
    return {status,{{"error",std::move(code)}}};
}

std::optional<session::SessionMemberRole> SessionRunApi::role(
    const identity::RuntimeSubject& subject,std::string_view session_id) const {
    if(!production_identity_valid(subject)) return {};
    auto product=catalog_.get(subject.tenant_id,session_id);
    if(!product||product->organization_id!=subject.organization_id||
       product->project_id!=subject.project_id||product->workspace_id!=subject.workspace_id)
        return {};
    auto member=catalog_.member(subject.tenant_id,session_id,subject.principal_id);
    if(!member) return {};
    return member->role;
}

ApiResult SessionRunApi::list_sessions(const identity::RuntimeSubject& subject,
                                       session::SessionListQuery query) const {
    if(!production_identity_valid(subject)) return denied("production_identity_required",401);
    if((!query.tenant_id.empty()&&query.tenant_id!=subject.tenant_id)||
       (!query.principal_id.empty()&&query.principal_id!=subject.principal_id))
        return denied("cross_scope_query_forbidden");
    query.tenant_id=subject.tenant_id;query.principal_id=subject.principal_id;
    query.organization_id=subject.organization_id;query.project_id=subject.project_id;
    query.workspace_id=subject.workspace_id;
    auto page=catalog_.list(query);json items=json::array();
    for(const auto& value:page.sessions) items.push_back(encode(value));
    return {200,{{"items",std::move(items)},
                 {"next_before_sequence",page.next_before_sequence}}};
}

ApiResult SessionRunApi::get_session(const identity::RuntimeSubject& subject,
                                     std::string_view session_id) const {
    if(!production_identity_valid(subject)) return denied("production_identity_required",401);
    if(!role(subject,session_id)) return denied("session_membership_required");
    auto value=catalog_.get(subject.tenant_id,session_id);
    return value?ApiResult{200,encode(*value)}:denied("session_not_found",404);
}

ApiResult SessionRunApi::create_session(const identity::RuntimeSubject& subject,
                                        session::ProductSession value) {
    if(!production_identity_valid(subject)) return denied("production_identity_required",401);
    if(value.tenant_id!=subject.tenant_id||value.organization_id!=subject.organization_id||
       value.project_id!=subject.project_id||value.workspace_id!=subject.workspace_id||
       value.owner_principal_id!=subject.principal_id)
        return denied("session_scope_mismatch");
    auto result=catalog_.create(value);
    if(!result.ok) return mutation(result);
    return {201,{{"created",true},{"session_id",value.session_id},
                 {"revision",result.revision}}};
}

ApiResult SessionRunApi::transition_session(const identity::RuntimeSubject& subject,
    std::string_view session_id,std::uint64_t expected,session::ProductSessionState state) {
    auto current=role(subject,session_id);
    if(!current) return production_identity_valid(subject)?denied("session_membership_required"):
        denied("production_identity_required",401);
    if(rank(*current)<rank(session::SessionMemberRole::Owner))
        return denied("session_owner_required");
    return mutation(catalog_.transition(subject.tenant_id,session_id,expected,state));
}

ApiResult SessionRunApi::enqueue_run(const identity::RuntimeSubject& subject,
                                     session::SessionRunRequest request) {
    auto current=role(subject,request.session_id);
    if(!current) return production_identity_valid(subject)?denied("session_membership_required"):
        denied("production_identity_required",401);
    if(rank(*current)<rank(session::SessionMemberRole::Operator))
        return denied("session_operator_required");
    if(request.tenant_id!=subject.tenant_id||request.organization_id!=subject.organization_id||
       request.project_id!=subject.project_id||request.principal_id!=subject.principal_id)
        return denied("run_scope_mismatch");
    return run_mutation(supervisor_.enqueue(std::move(request)));
}

ApiResult SessionRunApi::enqueue_command(const identity::RuntimeSubject& subject,
                                         session::SessionRunCommand command) {
    auto current=role(subject,command.session_id);
    if(!current) return production_identity_valid(subject)?denied("session_membership_required"):
        denied("production_identity_required",401);
    const int required=command.kind==session::SessionCommandKind::Comment?1:2;
    if(rank(*current)<required) return denied(required==1?"session_contributor_required":"session_operator_required");
    if(command.tenant_id!=subject.tenant_id) return denied("command_scope_mismatch");
    return run_mutation(supervisor_.enqueue_command(std::move(command)));
}

ApiResult SessionRunApi::get_run(const identity::RuntimeSubject& subject,
                                 std::string_view run_id) {
    if(!production_identity_valid(subject)) return denied("production_identity_required",401);
    auto value=supervisor_.load(subject.tenant_id,run_id);
    if(!value) return denied("run_not_found",404);
    if(!role(subject,value->request.session_id)) return denied("session_membership_required");
    return {200,encode(*value)};
}

ApiResult SessionRunApi::replay_events(const identity::RuntimeSubject& subject,
    std::string_view session_id,std::uint64_t after,std::size_t limit) {
    auto current=role(subject,session_id);
    if(!current) return production_identity_valid(subject)?denied("session_membership_required"):
        denied("production_identity_required",401);
    if(!events_) return denied("event_source_unavailable",503);
    if(limit==0||limit>500) return denied("invalid_event_limit",422);
    auto product=catalog_.get(subject.tenant_id,session_id);
    if(!product) return denied("session_not_found",404);
    const conversation::ConversationIdentity stream{subject.tenant_id,product->conversation_id};
    const auto head=events_->last_event_sequence(stream);
    const auto floor=events_->event_retention_floor(stream);
    if(after>head) return {409,{{"error","cursor_ahead_of_head"},{"head",head}}};
    if(floor>0&&after+1<floor) return {410,{{"error","cursor_expired"},{"floor",floor},{"head",head}}};
    json items=json::array();std::uint64_t next=after;
    for(const auto& event:events_->events(stream,after,limit)) {
        next=event.sequence;
        const bool visible=event.visibility==conversation::EventVisibility::User||
            (event.visibility==conversation::EventVisibility::Operations&&rank(*current)>=2)||
            (event.visibility==conversation::EventVisibility::Audit&&rank(*current)>=3);
        if(visible) items.push_back(conversation::encode(event));
    }
    return {200,{{"items",std::move(items)},{"after",after},{"next_cursor",next},
                 {"has_more",next<head},{"head",head},{"floor",floor}}};
}

ApiResult SessionRunApi::get_artifact(const identity::RuntimeSubject& subject,
    std::string_view session_id,std::string_view artifact_id) {
    if(!role(subject,session_id)) return production_identity_valid(subject)?
        denied("session_membership_required"):denied("production_identity_required",401);
    if(!interactions_) return denied("artifact_projection_unavailable",503);
    auto product=catalog_.get(subject.tenant_id,session_id);
    if(!product) return denied("session_not_found",404);
    auto snapshot=interactions_->snapshot(subject.tenant_id,product->conversation_id,
                                          ui::InteractionVisibility::User);
    if(!snapshot) return denied("artifact_projection_unavailable",404);
    for(const auto& node:snapshot->nodes)
        if(node.kind==ui::InteractionNodeKind::Artifact&&node.ref.artifact_id==artifact_id)
            return {200,ui::encode(node)};
    return denied("artifact_not_found",404);
}

ApiResult SessionRunApi::get_approval(const identity::RuntimeSubject& subject,
    std::string_view session_id,std::string_view approval_id) {
    auto current=role(subject,session_id);
    if(!current) return production_identity_valid(subject)?denied("session_membership_required"):
        denied("production_identity_required",401);
    if(rank(*current)<1) return denied("session_contributor_required");
    if(!approvals_) return denied("approval_source_unavailable",503);
    auto request=approvals_->request(approval_id);
    if(!request) return denied("approval_not_found",404);
    const auto& identity=request->metadata.identity;
    if(identity.tenant_id!=subject.tenant_id||identity.organization_id!=subject.organization_id||
       identity.project_id!=subject.project_id) return denied("approval_scope_mismatch");
    auto run=supervisor_.load(subject.tenant_id,identity.run_id);
    if(!run||run->request.session_id!=session_id) return denied("approval_run_scope_mismatch");
    json body={{"request",approval::encode(*request)}};
    if(auto decision=approvals_->latest_decision(approval_id))body["latest_decision"]=approval::encode(*decision);
    else body["latest_decision"]=nullptr;
    return {200,std::move(body)};
}

ApiResult SessionRunApi::capabilities(const identity::RuntimeSubject& subject,
                                      std::string_view session_id) {
    auto current=role(subject,session_id);
    if(!current) return production_identity_valid(subject)?denied("session_membership_required"):
        denied("production_identity_required",401);
    auto product=catalog_.get(subject.tenant_id,session_id);
    if(!product) return denied("session_not_found",404);
    json actions=json::array();
    auto add=[&](std::string id,std::string route,int required,bool source_ready=true,
                 std::string hint="button",bool session_revision=false) {
        const bool authorized=rank(*current)>=required;
        const bool enabled=authorized&&source_ready;
        actions.push_back({{"action_id",std::move(id)},{"route",std::move(route)},
            {"required_role",session::name(static_cast<session::SessionMemberRole>(required))},
            {"enabled",enabled},{"reason",enabled?"":(!authorized?"insufficient_session_role":"source_unavailable")},
            {"expected_revision",session_revision?json(product->revision):json(nullptr)},
            {"ui_hint",std::move(hint)}});
    };
    const auto base="/api/v1/sessions/"+std::string(session_id);
    add("session.view",base,0,true,"link");add("session.transition",base+"/state",3,true,"button",true);
    add("run.start","/api/v1/runs",2);add("run.comment","/api/v1/runs/{run_id}/commands",1);
    for(const auto* id:{"run.steer","run.queue","run.fork","run.cancel"})
        add(id,"/api/v1/runs/{run_id}/commands",2);
    add("events.replay",base+"/events",0,events_!=nullptr,"stream");
    add("events.stream",base+"/events/stream",0,events_!=nullptr,"stream");
    add("artifact.view",base+"/artifacts/{artifact_id}",0,interactions_!=nullptr,"link");
    add("approval.view",base+"/approvals/{approval_id}",1,approvals_!=nullptr,"link");
    actions.push_back({{"action_id","approval.decide"},{"route",base+"/approvals/{approval_id}/decisions"},
        {"required_role","operator"},{"enabled",false},{"reason","accountable_executor_required"},
        {"expected_revision",nullptr},{"ui_hint","hidden"}});
    return {200,{{"schema","agent.capability_manifest/v1"},{"session_id",session_id},
                 {"authorization_revision",subject.authorization_revision},
                 {"session_revision",product->revision},{"actions",std::move(actions)}}};
}

} // namespace agent_framework::api::v1
