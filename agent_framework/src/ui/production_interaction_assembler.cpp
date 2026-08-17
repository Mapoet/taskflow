#include "agent/ui/production_interaction_assembler.hpp"

#include <algorithm>
#include <set>

namespace agent_framework::ui {
namespace {
using json = nlohmann::json;

std::string digest(json value) {
    value.erase("canonical_digest"); value.erase("digest");
    return contracts::canonical_digest(value).value_or("sha256:unavailable");
}
bool same_scope(const contracts::ContractIdentity& a, const contracts::ContractIdentity& b) {
    return a.tenant_id == b.tenant_id && a.task_id == b.task_id && a.run_id == b.run_id;
}
InteractionObjectState child_state(std::string_view s) {
    if (s == "completed" || s == "succeeded" || s == "passed") return InteractionObjectState::Passed;
    if (s == "failed" || s == "cancelled") return InteractionObjectState::Failed;
    if (s == "waiting" || s == "blocked") return InteractionObjectState::Waiting;
    return InteractionObjectState::Running;
}
InteractionNode node(std::string id, InteractionNodeKind kind, const InteractionRef& ref,
                     std::string label, std::string summary, json display,
                     InteractionObjectState state, InteractionSourceRevision source,
                     std::string updated_at = {}) {
    InteractionNode n; n.node_id=std::move(id); n.kind=kind; n.ref=ref; n.label=std::move(label);
    n.summary=std::move(summary); n.display=std::move(display); n.state=state;
    n.visibility=InteractionVisibility::User; n.source=std::move(source);
    n.revision=std::max<std::uint64_t>(1,n.source.revision); n.updated_at=std::move(updated_at); return n;
}
InteractionEdge edge(std::string id, InteractionEdgeKind kind, std::string from, std::string to,
                     const InteractionSourceRevision& source) {
    InteractionEdge e; e.edge_id=std::move(id);e.kind=kind;e.from_node_id=std::move(from);e.to_node_id=std::move(to);
    e.revision=std::max<std::uint64_t>(1,source.revision);e.visibility=InteractionVisibility::User;e.source=source;return e;
}
void fail(ProductionInteractionResult& out, InteractionAssemblerError code, std::string text) {
    if(out.error==InteractionAssemblerError::None) out.error=code;
    out.diagnostics.push_back(std::move(text));
}
}

ProductionInteractionResult assemble_production_interactions(
    const ProductionInteractionStores& stores, const ProductionInteractionQuery& q) {
    ProductionInteractionResult out;
    if(q.identity.tenant_id.empty()||q.identity.task_id.empty()||q.conversation_id.empty()) {
        fail(out,InteractionAssemblerError::IdentityMismatch,"tenant, task and conversation identities are required");return out;
    }
    if((q.require_conversation&&!stores.conversations)||(q.require_run&&!stores.runs)||(q.require_plan&&!stores.plans)) {
        fail(out,InteractionAssemblerError::MissingRequiredStore,"required production store is not configured");return out;
    }
    InteractionSnapshot snapshot;snapshot.tenant_id=q.identity.tenant_id;snapshot.conversation_id=q.conversation_id;
    InteractionRef base;base.tenant_id=q.identity.tenant_id;base.conversation_id=q.conversation_id;base.turn_id=q.turn_id;
    base.message_id=q.message_id;base.task_id=q.identity.task_id;base.run_id=q.identity.run_id;base.plan_id=q.identity.plan_id;

    std::string message_node;
    if(stores.conversations) {
        conversation::ConversationIdentity cid{q.identity.tenant_id,q.conversation_id};
        auto messages=stores.conversations->messages(cid);
        auto it=q.message_id.empty()?messages.end():std::find_if(messages.begin(),messages.end(),[&](const auto&m){return m.message_id==q.message_id;});
        if(it==messages.end()&&!messages.empty()) it=std::prev(messages.end());
        if(it==messages.end()) { if(q.require_conversation) fail(out,InteractionAssemblerError::MissingRequiredObject,"conversation message not found"); }
        else {
            if(it->identity.tenant_id!=q.identity.tenant_id||it->identity.conversation_id!=q.conversation_id)
                fail(out,InteractionAssemblerError::IdentityMismatch,"conversation message scope mismatch");
            else { base.message_id=it->message_id;base.turn_id=it->turn_id;message_node="message:"+it->message_id;
                auto src=InteractionSourceRevision{"conversation",it->message_id,std::max<std::uint64_t>(1,it->sequence),it->digest.empty()?digest(conversation::encode(*it)):it->digest};
                snapshot.nodes.push_back(node(message_node,InteractionNodeKind::Message,base,"Original user request",it->content,
                    {{"role",it->role},{"text",it->content},{"turn_id",it->turn_id}},InteractionObjectState::Passed,src,it->created_at)); }
        }
    }
    if(stores.decisions) {
        auto decision=!q.decision_id.empty()
            ?stores.decisions->load(q.identity.tenant_id,q.decision_id)
            :stores.decisions->latest(q.identity.tenant_id,q.session_id,q.conversation_id);
        if(decision) {
            if(decision->subject.tenant_id!=q.identity.tenant_id||
               decision->subject.conversation_id!=q.conversation_id)
                fail(out,InteractionAssemblerError::IdentityMismatch,"decision scope mismatch");
            else {
                base.decision_id=decision->decision_id;
                auto options=json::array();
                for(const auto& option:decision->options)options.push_back({
                    {"id",option.option_id},{"label",option.label},
                    {"description",option.description},{"semantic_patch",option.semantic_patch}});
                const auto decision_json=json{{"kind",decision::name(decision->kind)},
                    {"question",decision->question},{"options",options},
                    {"selected_option_id",decision->selected_option_id},
                    {"state",decision::name(decision->state)}};
                auto src=InteractionSourceRevision{"decision",decision->decision_id,
                    decision->revision,digest(decision_json)};
                const auto waiting=decision->state==decision::DecisionState::Pending;
                const auto understanding_id="understanding:"+decision->decision_id;
                snapshot.nodes.push_back(node(understanding_id,InteractionNodeKind::Understanding,
                    base,"Task understanding",decision->question,
                    {{"decision_kind",decision::name(decision->kind)},
                     {"selected_option_id",decision->selected_option_id},
                     {"semantic_patch",decision->selected_option_id.empty()?json::object():
                        [&](){for(const auto&o:decision->options)if(o.option_id==decision->selected_option_id)return o.semantic_patch;return json::object();}()}},
                    waiting?InteractionObjectState::Waiting:InteractionObjectState::Passed,src,
                    decision->updated_at));
                const auto decision_id="decision:"+decision->decision_id;
                snapshot.nodes.push_back(node(decision_id,InteractionNodeKind::Decision,base,
                    "Decision required",decision->question,
                    {{"kind",decision::name(decision->kind)},{"options",options},
                     {"selected_option_id",decision->selected_option_id},
                     {"revision",decision->revision},{"expires_at_ms",decision->expires_at_ms}},
                    waiting?InteractionObjectState::Waiting:InteractionObjectState::Passed,src,
                    decision->updated_at));
                if(!message_node.empty())snapshot.edges.push_back(edge(
                    "edge:message-understanding:"+decision->decision_id,
                    InteractionEdgeKind::OriginatedFrom,message_node,understanding_id,src));
                snapshot.edges.push_back(edge("edge:understanding-decision:"+decision->decision_id,
                    InteractionEdgeKind::RequestedApproval,understanding_id,decision_id,src));
                snapshot.source_revisions.push_back(src);
            }
        }
    }
    std::string plan_node;
    if(stores.plans) {
        auto plan=stores.plans->current(q.identity);
        if(!plan) { if(q.require_plan) fail(out,InteractionAssemblerError::MissingRequiredObject,"current plan not found"); }
        else if(!same_scope(plan->metadata.identity,q.identity)) fail(out,InteractionAssemblerError::IdentityMismatch,"plan scope mismatch");
        else if(plan->plan_revision==0) fail(out,InteractionAssemblerError::RevisionInvalid,"plan revision must be positive");
        else { base.plan_id=plan->metadata.identity.plan_id;base.plan_revision=plan->plan_revision;plan_node="plan:"+base.plan_id;
            auto encoded=planning::encode(*plan);auto src=InteractionSourceRevision{"plan",base.plan_id,plan->plan_revision,digest(encoded)};
            snapshot.nodes.push_back(node(plan_node,InteractionNodeKind::Plan,base,"Execution plan r"+std::to_string(plan->plan_revision),
                std::to_string(plan->nodes.size())+" executable nodes",{{"critical_path",plan->critical_path},{"node_count",plan->nodes.size()}},InteractionObjectState::Running,src));
            for(const auto& p:plan->nodes){auto r=base;r.plan_node_id=p.node_id;
                auto id="plan-node:"+p.node_id;snapshot.nodes.push_back(node(id,InteractionNodeKind::PlanNode,r,p.objective,p.risk_level,
                    {{"dependencies",p.dependencies},{"required_capabilities",p.required_capabilities},{"approval_required",p.approval_required}},InteractionObjectState::Pending,src));
                snapshot.edges.push_back(edge("edge:"+plan_node+":"+id,InteractionEdgeKind::Implements,plan_node,id,src));}
            if(!message_node.empty()) snapshot.edges.push_back(edge("edge:message-plan",InteractionEdgeKind::PlannedBy,message_node,plan_node,src));
        }
    }
    if(stores.runs) {
        auto run=stores.runs->load(q.identity.run_id);
        if(!run) { if(q.require_run) fail(out,InteractionAssemblerError::MissingRequiredObject,"run checkpoint not found"); }
        else if(!same_scope(run->checkpoint.metadata.identity,q.identity)) fail(out,InteractionAssemblerError::IdentityMismatch,"run scope mismatch");
        else if(run->revision==0) fail(out,InteractionAssemblerError::RevisionInvalid,"run revision must be positive");
        else { snapshot.revision=std::max(snapshot.revision,run->revision);snapshot.source_revisions.push_back({"run",q.identity.run_id,run->revision,digest(run::encode(run->checkpoint))}); }
    }
    if(stores.sessions&&!q.session_id.empty()) {
        auto session=stores.sessions->load_current(q.session_id);
        if(session) {auto src=InteractionSourceRevision{"session",q.session_id,session->revision,digest(agent_thread_state_to_json(session->state))};
            std::string parent;
            for(const auto& child:session->child_tasks){auto r=base;r.agent_invocation_id=child.child_id;r.child_agent_id=child.child_id;
                const auto id="agent:"+child.child_id;snapshot.nodes.push_back(node(id,InteractionNodeKind::Agent,r,child.child_id,child.status,
                    {{"backend",child.backend},{"attempt",child.attempt},{"status",child.status}},child_state(child.status),src));
                if(!parent.empty())snapshot.edges.push_back(edge("edge:child:"+parent+":"+id,InteractionEdgeKind::ParentOf,parent,id,src));
                else if(!plan_node.empty())snapshot.edges.push_back(edge("edge:plan-child:"+id,InteractionEdgeKind::DelegatedTo,plan_node,id,src));
                parent=id;}
            for(const auto& tool:session->tool_commits){auto r=base;r.tool_invocation_id=tool.tool_call_id;auto id="tool:"+tool.tool_call_id;
                snapshot.nodes.push_back(node(id,InteractionNodeKind::ToolInvocation,r,tool.tool_call_id,tool.status,
                    {{"attempt",tool.attempt},{"result_digest",tool.result_digest}},child_state(tool.status),src));
                if(!parent.empty())snapshot.edges.push_back(edge("edge:agent-tool:"+id,InteractionEdgeKind::Produced,parent,id,src));}
            snapshot.source_revisions.push_back(src);
        }
    }
    if(stores.approvals&&!q.approval_id.empty()) {
        auto request=stores.approvals->request(q.approval_id);
        if(request) {if(!same_scope(request->metadata.identity,q.identity))fail(out,InteractionAssemblerError::IdentityMismatch,"approval scope mismatch");
            else {auto decision=stores.approvals->latest_decision(q.approval_id);auto r=base;r.approval_id=q.approval_id;
                auto src=InteractionSourceRevision{"approval",q.approval_id,decision?2u:1u,digest(approval::encode(*request))};
                auto state=decision?InteractionObjectState::Passed:InteractionObjectState::Waiting;
                snapshot.nodes.push_back(node("approval:"+q.approval_id,InteractionNodeKind::Approval,r,request->request_kind,request->reason,
                    {{"scope",request->scope},{"risk_level",request->risk_level},{"expires_at",request->expires_at},{"policy_revision",request->policy_revision},
                     {"allowed_actions",json::array({"approve","reject","edit","delegate","escalate"})},{"decided",decision.has_value()}},state,src,request->created_at));
                if(!message_node.empty())snapshot.edges.push_back(edge("edge:message-approval:"+q.approval_id,InteractionEdgeKind::RequestedApproval,message_node,"approval:"+q.approval_id,src));
                if(!plan_node.empty())snapshot.edges.push_back(edge("edge:approval-plan:"+q.approval_id,InteractionEdgeKind::Resumes,"approval:"+q.approval_id,plan_node,src));
                snapshot.source_revisions.push_back(src);}
        }
    }
    if(stores.assurance&&!q.assurance_workflow_id.empty()) {
        auto cp=stores.assurance->load_checkpoint(q.identity.tenant_id,q.assurance_workflow_id);
        if(cp) {if(!same_scope(cp->checkpoint.metadata.identity,q.identity))fail(out,InteractionAssemblerError::IdentityMismatch,"assurance scope mismatch");
            else {auto src=InteractionSourceRevision{"assurance",q.assurance_workflow_id,cp->revision,digest(assurance::encode(cp->checkpoint))};
                for(const auto& artifact:cp->checkpoint.artifacts){auto id="artifact:"+artifact.output_digest;auto r=base;r.artifact_id=artifact.output_digest;
                    snapshot.nodes.push_back(node(id,InteractionNodeKind::Artifact,r,assurance::assurance_stage_name(artifact.stage),artifact.output_digest,
                        {{"provider",artifact.provider},{"model",artifact.model},{"invocation_id",artifact.invocation_id}},InteractionObjectState::Passed,src));
                    if(!plan_node.empty())snapshot.edges.push_back(edge("edge:assurance:"+id,InteractionEdgeKind::VerifiedBy,plan_node,id,src));}
                snapshot.source_revisions.push_back(src);}
        }
    }
    if(stores.remediation&&!q.remediation_workflow_id.empty()) {
        auto cp=stores.remediation->load(q.identity.tenant_id,q.remediation_workflow_id);
        if(cp) {if(!same_scope(cp->checkpoint.metadata.identity,q.identity))fail(out,InteractionAssemblerError::IdentityMismatch,"remediation scope mismatch");
            else {auto src=InteractionSourceRevision{"remediation",q.remediation_workflow_id,cp->revision,digest(remediation::encode(cp->checkpoint))};
                std::set<std::string> finding_nodes;
                if(cp->checkpoint.remediation_plan)for(const auto&a:cp->checkpoint.remediation_plan->actions){auto r=base;r.plan_node_id=a.action_id;
                    auto id="plan-node:remediation:"+a.action_id;snapshot.nodes.push_back(node(id,InteractionNodeKind::PlanNode,r,"Remediation · "+a.objective,a.risk_level,
                        {{"finding_ids",a.finding_ids},{"affected_artifact_ids",a.affected_artifact_ids},{"approval_required",a.approval_required}},InteractionObjectState::Pending,src));
                    if(!plan_node.empty())snapshot.edges.push_back(edge("edge:remediation:"+id,InteractionEdgeKind::Supersedes,plan_node,id,src));
                    for(const auto& finding:a.finding_ids){auto fid="finding:"+finding;if(finding_nodes.insert(fid).second){auto fr=base;fr.finding_id=finding;
                        snapshot.nodes.push_back(node(fid,InteractionNodeKind::Finding,fr,"Finding requiring remediation",finding,{{"remediation_action",a.action_id}},InteractionObjectState::Warning,src));}
                        snapshot.edges.push_back(edge("edge:finding-action:"+finding+":"+a.action_id,InteractionEdgeKind::Supports,fid,id,src));}}
                for(const auto& artifact:cp->checkpoint.artifacts){auto aid="artifact:remediation:"+artifact.output_digest;auto ar=base;ar.artifact_id=artifact.output_digest;
                    snapshot.nodes.push_back(node(aid,InteractionNodeKind::Artifact,ar,"Remediation stage artifact",remediation::remediation_stage_name(artifact.stage),
                        {{"provider",artifact.provider},{"model",artifact.model},{"invocation_id",artifact.invocation_id},{"attempt",artifact.attempt}},InteractionObjectState::Passed,src));
                    if(!plan_node.empty())snapshot.edges.push_back(edge("edge:remediation-artifact:"+artifact.output_digest,InteractionEdgeKind::Produced,plan_node,aid,src));}
                if(cp->checkpoint.reverification_plan){const auto& rv=*cp->checkpoint.reverification_plan;auto rr=base;rr.plan_node_id=rv.reverification_id;
                    auto rid="plan-node:reverification:"+rv.reverification_id;snapshot.nodes.push_back(node(rid,InteractionNodeKind::PlanNode,rr,"Reverification plan",std::to_string(rv.criterion_ids.size())+" criteria",
                        {{"criterion_ids",rv.criterion_ids},{"forced_oracle_kinds",rv.forced_oracle_kinds},{"invalidated_evidence_ids",rv.invalidated_evidence_ids},{"reusable_evidence_ids",rv.reusable_evidence_ids}},InteractionObjectState::Pending,src));
                    if(!plan_node.empty())snapshot.edges.push_back(edge("edge:reverification:"+rid,InteractionEdgeKind::VerifiedBy,plan_node,rid,src));}
                if(cp->checkpoint.state==remediation::RemediationState::ReadyForExecution){auto cr=base;auto cid="closure:"+q.identity.run_id;
                    snapshot.nodes.push_back(node(cid,InteractionNodeKind::Closure,cr,"Remediation ready for execution",cp->checkpoint.committed_plan_digest,
                        {{"workflow_id",q.remediation_workflow_id},{"committed_plan_digest",cp->checkpoint.committed_plan_digest}},InteractionObjectState::Passed,src));
                    if(!plan_node.empty())snapshot.edges.push_back(edge("edge:closure:"+cid,InteractionEdgeKind::ClosedBy,plan_node,cid,src));}
                snapshot.source_revisions.push_back(src);}
        }
    }
    if(out.error!=InteractionAssemblerError::None)return out;
    std::set<std::string> ids;for(const auto& n:snapshot.nodes)if(!ids.insert(n.node_id).second)fail(out,InteractionAssemblerError::AmbiguousObject,"duplicate node id: "+n.node_id);
    for(const auto& n:snapshot.nodes)if(!validate(n).empty())fail(out,InteractionAssemblerError::UnsafeDisplayData,"invalid interaction node: "+n.node_id);
    if(out.error!=InteractionAssemblerError::None)return out;
    for(const auto& n:snapshot.nodes)snapshot.source_revisions.push_back(n.source);
    std::sort(snapshot.source_revisions.begin(),snapshot.source_revisions.end(),[](const auto&a,const auto&b){return std::tie(a.store,a.object_id,a.revision)<std::tie(b.store,b.object_id,b.revision);});
    snapshot.source_revisions.erase(std::unique(snapshot.source_revisions.begin(),snapshot.source_revisions.end(),[](const auto&a,const auto&b){return a.store==b.store&&a.object_id==b.object_id&&a.revision==b.revision;}),snapshot.source_revisions.end());
    snapshot.revision=std::max<std::uint64_t>(1,snapshot.revision);snapshot.updated_at=q.now;snapshot.digest=encode(snapshot).at("digest");out.snapshot=std::move(snapshot);return out;
}
} // namespace agent_framework::ui
