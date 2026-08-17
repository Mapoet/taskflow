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
        {"command_cursor",value.command_cursor},
        {"lease_expires_at_ms",value.lease_expires_at_ms},
        {"created_at",value.request.created_at},{"updated_at",value.updated_at}};
}

ApiResult mutation(const session::SessionMutationResult& result) {
    if(result.ok) return {200,{{"ok",true},{"revision",result.revision}}};
    const int status=result.error=="revision_conflict"?409:
        result.error=="resource_not_found"?404:422;
    json body={{"error",result.error},{"revision",result.revision}};
    if(status==409){body["changed_fields"]={"session_revision"};
        body["safe_retry"]="refresh_and_reapply_if_intent_still_valid";}
    return {status,std::move(body)};
}

ApiResult run_mutation(const session::RunSupervisorResult& result, int accepted=202) {
    if(result.ok) return {accepted,{{"accepted",true},{"revision",result.revision},
                                    {"lease_epoch",result.lease_epoch}}};
    const int status=result.error.find("conflict")!=std::string::npos?409:
        result.error.find("not_found")!=std::string::npos?404:422;
    json body={{"error",result.error},{"revision",result.revision}};
    if(status==409){body["changed_fields"]={"run_revision"};
        body["safe_retry"]="refresh_and_reapply_if_intent_still_valid";}
    return {status,std::move(body)};
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

ApiResult SessionRunApi::fork_session(const identity::RuntimeSubject& subject,
    std::string_view source_session_id,std::uint64_t expected_source_revision,
    session::ProductSession forked) {
    auto current=role(subject,source_session_id);
    if(!current)return production_identity_valid(subject)?denied("session_membership_required"):
        denied("production_identity_required",401);
    if(rank(*current)<rank(session::SessionMemberRole::Operator))
        return denied("session_operator_required");
    const auto source=catalog_.get(subject.tenant_id,source_session_id);
    if(!source)return denied("session_not_found",404);
    if(source->revision!=expected_source_revision)
        return mutation({false,source->revision,"revision_conflict"});
    if(forked.session_id.empty()||forked.conversation_id.empty()||
       forked.session_id==source->session_id||forked.conversation_id==source->conversation_id)
        return denied("fork_identity_must_be_new",422);
    forked.tenant_id=subject.tenant_id;forked.organization_id=subject.organization_id;
    forked.project_id=subject.project_id;forked.workspace_id=subject.workspace_id;
    forked.owner_principal_id=subject.principal_id;
    if(forked.title.empty())forked.title=source->title+" (fork)";
    if(forked.folder.empty())forked.folder=source->folder;
    if(forked.tags.empty())forked.tags=source->tags;
    const auto created=catalog_.create(forked);
    if(!created.ok)return mutation(created);
    return {201,{{"created",true},{"source_session_id",source_session_id},
        {"source_revision",source->revision},{"session_id",forked.session_id},
        {"conversation_id",forked.conversation_id},{"revision",created.revision}}};
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

ApiResult SessionRunApi::restore_session(const identity::RuntimeSubject& subject,
    std::string_view session_id,std::uint64_t expected) {
    return transition_session(subject,session_id,expected,session::ProductSessionState::Active);
}

ApiResult SessionRunApi::purge_session(const identity::RuntimeSubject& subject,
    std::string_view session_id,std::uint64_t expected,bool confirm_permanent) {
    auto current=role(subject,session_id);
    if(!current)return production_identity_valid(subject)?denied("session_membership_required"):
        denied("production_identity_required",401);
    if(rank(*current)<3)return denied("session_owner_required");
    auto product=catalog_.get(subject.tenant_id,session_id);
    if(!product)return denied("session_not_found",404);
    if(product->revision!=expected)return mutation({false,product->revision,"revision_conflict"});
    if(product->state==session::ProductSessionState::Trashed&&!confirm_permanent)
        return mutation(catalog_.transition(subject.tenant_id,session_id,expected,
                                             session::ProductSessionState::PurgePending));
    if(product->state==session::ProductSessionState::PurgePending&&confirm_permanent)
        return mutation(catalog_.transition(subject.tenant_id,session_id,expected,
                                             session::ProductSessionState::Purged));
    return {422,{{"error",confirm_permanent?"purge_confirmation_state_invalid":
        "session_must_be_trashed_before_purge"},{"revision",product->revision},
        {"state",session::name(product->state)}}};
}

ApiResult SessionRunApi::rename_session(const identity::RuntimeSubject& subject,
    std::string_view session_id,std::uint64_t expected,std::string_view title) {
    auto current=role(subject,session_id);
    if(!current)return production_identity_valid(subject)?denied("session_membership_required"):
        denied("production_identity_required",401);
    if(rank(*current)<2)return denied("session_operator_required");
    return mutation(catalog_.rename(subject.tenant_id,session_id,expected,title));
}

ApiResult SessionRunApi::organize_session(const identity::RuntimeSubject& subject,
    std::string_view session_id,std::uint64_t expected,std::string_view folder,
    const std::vector<std::string>& tags,bool pinned) {
    auto current=role(subject,session_id);
    if(!current)return production_identity_valid(subject)?denied("session_membership_required"):
        denied("production_identity_required",401);
    if(rank(*current)<2)return denied("session_operator_required");
    return mutation(catalog_.organize(subject.tenant_id,session_id,expected,folder,tags,pinned));
}

ApiResult SessionRunApi::put_session_member(const identity::RuntimeSubject& subject,
    std::string_view session_id,std::uint64_t expected,std::string_view principal,
    session::SessionMemberRole member_role) {
    auto current=role(subject,session_id);
    if(!current)return production_identity_valid(subject)?denied("session_membership_required"):
        denied("production_identity_required",401);
    if(rank(*current)<3)return denied("session_owner_required");
    if(principal.empty())return denied("member_principal_required",422);
    return mutation(catalog_.put_member({subject.tenant_id,std::string(session_id),
        std::string(principal),member_role},expected));
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

ApiResult SessionRunApi::get_execution_snapshot(const identity::RuntimeSubject& subject,
    std::string_view session_id,std::string_view task_id,std::string_view requested_run_id) {
    if(!role(subject,session_id))return production_identity_valid(subject)?
        denied("session_membership_required"):denied("production_identity_required",401);
    if(!tasks_||!events_||!interactions_)
        return denied("execution_snapshot_sources_unavailable",503);
    const auto product=catalog_.get(subject.tenant_id,session_id);
    if(!product)return denied("session_not_found",404);
    const conversation::ConversationIdentity identity{subject.tenant_id,product->conversation_id};
    const auto task=tasks_->load(identity,task_id);
    if(!task)return denied("task_not_found",404);
    const auto requirements=tasks_->requirements(identity,task_id);
    const auto links=tasks_->runs(identity,task_id);
    const conversation::TaskRunLink* link=nullptr;
    for(auto it=links.rbegin();it!=links.rend();++it)
        if(requested_run_id.empty()||it->run_id==requested_run_id){link=&*it;break;}
    const std::string run_id=requested_run_id.empty()?
        (link?link->run_id:task->current_run_id):std::string(requested_run_id);
    if(run_id.empty())return denied("task_run_not_bound",409);
    const auto run=supervisor_.load(subject.tenant_id,run_id);
    if(!run||run->request.session_id!=session_id)return denied("task_run_scope_mismatch",409);
    const auto turn=task->current_turn_id.empty()?std::optional<conversation::TurnCheckpoint>{}:
        events_->load_turn(identity,task->current_turn_id);
    const auto projection=interactions_->snapshot(subject.tenant_id,product->conversation_id,
                                                   ui::InteractionVisibility::User);
    conversation::TaskExecutionSnapshot snapshot;
    snapshot.tenant_id=subject.tenant_id;snapshot.session_id=std::string(session_id);
    snapshot.conversation_id=product->conversation_id;snapshot.task_id=task->task_id;
    snapshot.run_id=run_id;snapshot.turn_id=task->current_turn_id;
    snapshot.task_revision=task->revision;snapshot.requirement_revision=task->requirement_revision;
    snapshot.plan_revision=link?link->plan_revision:task->plan_revision;
    snapshot.run_revision=run->revision;snapshot.turn_revision=turn?turn->revision:0;
    snapshot.projection_revision=projection?projection->revision:0;
    snapshot.task_digest=task->digest;
    if(!requirements.empty())snapshot.requirement_digest=requirements.back().digest;
    if(link){snapshot.plan_digest=link->plan_digest;snapshot.plan_id=
        link->plan_revision?"plan:"+task->task_id+":"+std::to_string(link->plan_revision):"";}
    snapshot.run_digest=contracts::canonical_digest(encode(*run)).value_or("");
    if(projection)snapshot.projection_digest=projection->digest.empty()?
        contracts::canonical_digest(ui::encode(*projection)).value_or(""):projection->digest;
    auto body=conversation::encode(snapshot);
    body["event_head"]=events_->last_event_sequence(identity);
    return {200,std::move(body)};
}

ApiResult SessionRunApi::get_task(const identity::RuntimeSubject& subject,
    std::string_view session_id,std::string_view task_id) {
    if(!role(subject,session_id))return production_identity_valid(subject)?
        denied("session_membership_required"):denied("production_identity_required",401);
    if(!tasks_)return denied("task_registry_unavailable",503);
    const auto product=catalog_.get(subject.tenant_id,session_id);
    if(!product)return denied("session_not_found",404);
    const conversation::ConversationIdentity identity{subject.tenant_id,product->conversation_id};
    const auto task=tasks_->load(identity,task_id);
    if(!task)return denied("task_not_found",404);
    json requirements=json::array();
    for(const auto& revision:tasks_->requirements(identity,task_id))
        requirements.push_back(conversation::encode(revision));
    json runs=json::array();
    for(const auto& link:tasks_->runs(identity,task_id))
        runs.push_back({{"run_id",link.run_id},{"requirement_revision",link.requirement_revision},
            {"plan_revision",link.plan_revision},{"state",link.state},
            {"plan_digest",link.plan_digest},{"task_contract_digest",link.task_contract_digest},
            {"classification_decision_id",link.classification_decision_id},
            {"clarification_id",link.clarification_id},{"created_at",link.created_at},
            {"updated_at",link.updated_at}});
    return {200,{{"schema","agent.task_aggregate/v1"},{"session_id",session_id},
        {"conversation_id",product->conversation_id},{"task",conversation::encode(*task)},
        {"requirements",std::move(requirements)},{"runs",std::move(runs)}}};
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

ApiResult SessionRunApi::get_session_data(const identity::RuntimeSubject& subject,
    std::string_view session_id,std::uint64_t after,std::size_t limit) {
    auto product=get_session(subject,session_id);
    if(!product.ok())return product;
    auto replay=replay_events(subject,session_id,after,limit);
    if(!replay.ok())return replay;
    json body={{"schema","agent.session_data_page/v1"},{"session",std::move(product.body)},
        {"events",std::move(replay.body)}};
    if(interactions_) {
        const auto projected=get_interactions(subject,session_id,ui::InteractionVisibility::User);
        body["interactions"]=projected.ok()?projected.body:json(nullptr);
    } else body["interactions"]=nullptr;
    return {200,std::move(body)};
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

ApiResult SessionRunApi::get_interactions(const identity::RuntimeSubject& subject,
    std::string_view session_id,ui::InteractionVisibility viewer) {
    auto current=role(subject,session_id);
    if(!current)return production_identity_valid(subject)?denied("session_membership_required"):
        denied("production_identity_required",401);
    if(viewer==ui::InteractionVisibility::Operations&&rank(*current)<2)
        return denied("session_operator_required");
    if(viewer==ui::InteractionVisibility::Audit&&rank(*current)<3)
        return denied("session_owner_required");
    if(!interactions_)return denied("interaction_projection_unavailable",503);
    auto product=catalog_.get(subject.tenant_id,session_id);
    if(!product)return denied("session_not_found",404);
    auto snapshot=interactions_->snapshot(subject.tenant_id,product->conversation_id,viewer);
    if(!snapshot)return denied("interaction_projection_not_built",404);
    auto body=ui::encode(*snapshot);
    const auto runtime_head=events_?events_->last_event_sequence(
        {subject.tenant_id,product->conversation_id}):snapshot->head_sequence;
    body["runtime_event_head"]=runtime_head;
    body["stale"]=snapshot->head_sequence!=runtime_head;
    return {200,std::move(body)};
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

ApiResult SessionRunApi::get_decision(const identity::RuntimeSubject& subject,
    std::string_view session_id,std::string_view decision_id) {
    auto current=role(subject,session_id);
    if(!current)return production_identity_valid(subject)?denied("session_membership_required"):
        denied("production_identity_required",401);
    if(!decisions_)return denied("decision_source_unavailable",503);
    const auto value=decisions_->load(subject.tenant_id,decision_id);
    if(!value)return denied("decision_not_found",404);
    if(value->subject.session_id!=session_id||value->subject.conversation_id.empty())
        return denied("decision_session_scope_mismatch");
    json options=json::array();for(const auto& option:value->options)options.push_back({
        {"id",option.option_id},{"label",option.label},{"description",option.description}});
    return {200,{{"decision_id",value->decision_id},{"kind",decision::name(value->kind)},
        {"state",decision::name(value->state)},{"question",value->question},
        {"options",std::move(options)},{"selected_option_id",value->selected_option_id},
        {"recommended_option_id",value->recommended_option_id},{"revision",value->revision},
        {"expires_at_ms",value->expires_at_ms}}};
}

ApiResult SessionRunApi::answer_decision(const identity::RuntimeSubject& subject,
    std::string_view session_id,std::string_view decision_id,std::uint64_t expected,
    std::string_view option_id,std::uint64_t now_ms) {
    auto current=role(subject,session_id);
    if(!current)return production_identity_valid(subject)?denied("session_membership_required"):
        denied("production_identity_required",401);
    if(rank(*current)<1)return denied("session_contributor_required");
    if(!decisions_)return denied("decision_source_unavailable",503);
    const auto value=decisions_->load(subject.tenant_id,decision_id);
    if(!value)return denied("decision_not_found",404);
    if(value->subject.session_id!=session_id)return denied("decision_session_scope_mismatch");
    const auto run=supervisor_.load(subject.tenant_id,value->subject.run_id);
    if(!run||run->request.session_id!=session_id)return denied("decision_run_scope_mismatch",409);
    const auto result=decisions_->answer(subject.tenant_id,decision_id,expected,option_id,now_ms);
    const auto replay=!result.ok&&value->state==decision::DecisionState::Answered&&
        value->selected_option_id==option_id&&value->revision==expected+1;
    if(result.ok||replay) {
        session::SessionRunCommand resume;resume.tenant_id=subject.tenant_id;
        resume.session_id=std::string(session_id);resume.run_id=value->subject.run_id;
        resume.command_id="decision-answer:"+std::string(decision_id)+":"+std::to_string(expected);
        resume.kind=session::SessionCommandKind::Steer;
        resume.payload={{"decision_id",decision_id},{"option_id",option_id},
            {"decision_revision",result.ok?result.revision:value->revision}};
        resume.expected_run_revision=run->revision;
        const auto queued=supervisor_.enqueue_command(std::move(resume));
        if(!queued.ok)return {503,{{"error","decision_resume_command_failed"},
            {"detail",queued.error},{"retryable",true}}};
        return {200,{{"answered",true},{"replayed",replay},
            {"revision",result.ok?result.revision:value->revision},
            {"state","answered"},{"resume_command_sequence",queued.revision}}};
    }
    const int status=result.error.find("revision")!=std::string::npos?409:
        result.error=="decision_expired"?410:422;
    return {status,{{"error",result.error},{"revision",result.revision},
        {"state",decision::name(result.state)}}};
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
    add("session.view",base,0,true,"link");
    add("session.rename",base+"/title",2,true,"button",true);
    add("session.organize",base+"/organization",2,true,"button",true);
    add("session.member.put",base+"/members/{principal_id}",3,true,"button");
    add("session.transition",base+"/state",3,true,"button",true);
    add("session.restore",base+"/restore",3,true,"button",true);
    add("session.purge",base+"/purge",3,true,"button",true);
    add("session.fork",base+"/fork",2,true,"button",true);
    add("session.data.view",base+"/data",0,events_!=nullptr,"link");
    add("run.start","/api/v1/runs",2);add("run.comment","/api/v1/runs/{run_id}/commands",1);
    for(const auto* id:{"run.steer","run.queue","run.fork","run.cancel",
                        "run.retry","run.reconcile","run.escalate"})
        add(id,"/api/v1/runs/{run_id}/commands",2);
    add("events.replay",base+"/events",0,events_!=nullptr,"stream");
    add("events.stream",base+"/events/stream",0,events_!=nullptr,"stream");
    add("interaction.view",base+"/interactions",0,interactions_!=nullptr,"link");
    add("execution_snapshot.view",base+"/tasks/{task_id}/execution-snapshot",0,
        tasks_!=nullptr&&events_!=nullptr&&interactions_!=nullptr,"link");
    add("task.view",base+"/tasks/{task_id}",0,tasks_!=nullptr,"link");
    add("artifact.view",base+"/artifacts/{artifact_id}",0,interactions_!=nullptr,"link");
    add("approval.view",base+"/approvals/{approval_id}",1,approvals_!=nullptr,"link");
    add("decision.view",base+"/decisions/{decision_id}",0,decisions_!=nullptr,"link");
    add("decision.answer",base+"/decisions/{decision_id}/answer",1,decisions_!=nullptr,"button");
    actions.push_back({{"action_id","approval.decide"},{"route",base+"/approvals/{approval_id}/decisions"},
        {"required_role","operator"},{"enabled",false},{"reason","accountable_executor_required"},
        {"expected_revision",nullptr},{"ui_hint","hidden"}});
    return {200,{{"schema","agent.capability_manifest/v1"},{"session_id",session_id},
                 {"authorization_revision",subject.authorization_revision},
                 {"session_revision",product->revision},{"actions",std::move(actions)}}};
}

} // namespace agent_framework::api::v1
