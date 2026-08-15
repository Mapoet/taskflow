#include "agent/ui/interaction_source_adapters.hpp"

#include <algorithm>

namespace agent_framework::ui {
namespace {
InteractionObjectState state(OperationsStatus value) {
    switch(value) {
    case OperationsStatus::Pending: return InteractionObjectState::Pending;
    case OperationsStatus::Running: return InteractionObjectState::Running;
    case OperationsStatus::Passed: return InteractionObjectState::Passed;
    case OperationsStatus::Warning: return InteractionObjectState::Warning;
    case OperationsStatus::Blocked: return InteractionObjectState::Blocked;
    case OperationsStatus::Failed: return InteractionObjectState::Failed;
    default: return InteractionObjectState::Unavailable;
    }
}
InteractionSourceRevision source(const Phase4OperationsSnapshot& s,std::string id) {
    auto found=std::find_if(s.source_revisions.begin(),s.source_revisions.end(),
        [&](const auto&r){return r.object_id==id;});
    if(found!=s.source_revisions.end()) return {found->store,found->object_id,
        std::max<std::uint64_t>(1,found->revision),found->digest.empty()?"sha256:unavailable":found->digest};
    return {"phase4_operations",std::move(id),std::max<std::uint64_t>(1,s.plan_revision),
        "sha256:"+s.snapshot_id};
}
InteractionRef base(const Phase4OperationsSnapshot&s,const InteractionProjectionContext&c) {
    InteractionRef r; r.tenant_id=s.tenant_id; r.conversation_id=c.conversation_id;
    r.turn_id=c.turn_id; r.message_id=c.user_message_id; r.task_id=s.task_id;
    r.run_id=s.run_id; r.harness_id="harness:"+s.run_id; return r;
}
InteractionEdge edge(const Phase4OperationsSnapshot&s,std::string id,InteractionEdgeKind kind,
                     std::string from,std::string to,InteractionVisibility visibility=InteractionVisibility::User) {
    InteractionEdge e; e.edge_id=std::move(id);e.kind=kind;e.from_node_id=std::move(from);
    e.to_node_id=std::move(to);e.revision=std::max<std::uint64_t>(1,s.plan_revision);
    e.visibility=visibility;e.source=source(s,e.edge_id);e.updated_at=s.updated_at;return e;
}
}

InteractionSnapshot project_interactions(const Phase4OperationsSnapshot&s,
                                          const InteractionProjectionContext&c) {
    InteractionSnapshot out;out.tenant_id=s.tenant_id;out.conversation_id=c.conversation_id;
    out.revision=std::max<std::uint64_t>(1,s.plan_revision);out.updated_at=s.updated_at;
    auto ref=base(s,c);
    InteractionNode message;message.node_id="message:"+c.user_message_id;message.kind=InteractionNodeKind::Message;
    message.ref=ref;message.label="Original user request";message.summary=c.user_message_summary.empty()?s.summary:c.user_message_summary;
    message.display={{"role","user"},{"text",message.summary},{"turn_id",c.turn_id}};
    message.state=InteractionObjectState::Passed;message.visibility=InteractionVisibility::User;
    message.source=source(s,c.user_message_id);message.updated_at=s.updated_at;out.nodes.push_back(message);

    InteractionNode thinking;thinking.node_id="thinking:"+c.turn_id;thinking.kind=InteractionNodeKind::Thinking;
    thinking.ref=ref;thinking.label="Cognition & planning summary";thinking.summary=s.summary;
    thinking.display={{"unknowns",s.unknowns},{"method","Evidence-grounded cognition → plan → execution → assurance"},
        {"privacy","Display-safe reasoning summary; hidden chain-of-thought is not retained."}};
    thinking.state=state(s.overall_status);thinking.visibility=InteractionVisibility::User;
    thinking.source=source(s,"thinking:"+c.turn_id);thinking.updated_at=s.updated_at;out.nodes.push_back(thinking);
    out.edges.push_back(edge(s,"edge:message-thinking",InteractionEdgeKind::PlannedBy,message.node_id,thinking.node_id));

    InteractionNode plan;plan.node_id="plan:"+s.task_id;plan.kind=InteractionNodeKind::Plan;plan.ref=ref;
    plan.ref.plan_id=s.task_id+":plan";plan.ref.plan_revision=std::max<std::uint64_t>(1,s.plan_revision);
    plan.label="Execution plan r"+std::to_string(plan.ref.plan_revision);plan.summary=s.summary;
    plan.display={{"criteria_closed",s.criteria_closed},{"criteria_total",s.criteria_total},
        {"progress_delta",s.progress_delta},{"stagnation_count",s.stagnation_count},{"blocker",s.blocker}};
    plan.state=state(s.overall_status);plan.visibility=InteractionVisibility::User;
    plan.source=source(s,plan.ref.plan_id);plan.updated_at=s.updated_at;out.nodes.push_back(plan);
    out.edges.push_back(edge(s,"edge:thinking-plan",InteractionEdgeKind::PlannedBy,thinking.node_id,plan.node_id));

    for(const auto&stage:s.stages){InteractionNode n;n.node_id="plan-node:"+stage.id;n.kind=InteractionNodeKind::PlanNode;
        n.ref=plan.ref;n.ref.plan_node_id=stage.id;n.label=stage.label;n.summary=stage.summary;
        n.display={{"role",stage.role},{"evidence_ids",stage.evidence_ids},{"stage_revision",stage.revision}};
        n.state=state(stage.status);n.visibility=InteractionVisibility::User;n.source=source(s,stage.id);n.updated_at=s.updated_at;
        out.nodes.push_back(n);out.edges.push_back(edge(s,"edge:plan:"+stage.id,InteractionEdgeKind::Implements,plan.node_id,n.node_id));}

    InteractionNode memory;memory.node_id="memory:"+s.task_id;memory.kind=InteractionNodeKind::MemoryView;memory.ref=ref;
    memory.ref.memory_snapshot_id=s.snapshot_id+":memory";memory.ref.memory_view_digest="sha256:"+s.snapshot_id+":memory";
    memory.label="Five-layer memory view";memory.summary=std::to_string(s.memory.size())+" governed memory references";
    memory.display=nlohmann::json{{"layers",nlohmann::json::array()},{"selected_count",0}};
    std::size_t selected=0;for(const auto&m:s.memory){memory.display["layers"].push_back({{"id",m.id},{"scope",m.scope},{"source",m.source},
        {"authority",m.authority},{"freshness",m.freshness},{"conflict",m.conflict},{"selected",m.selected},{"selection_reason",m.selection_reason}});if(m.selected)++selected;}
    memory.display["selected_count"]=selected;memory.state=InteractionObjectState::Passed;memory.visibility=InteractionVisibility::User;
    memory.source=source(s,memory.ref.memory_snapshot_id);memory.updated_at=s.updated_at;out.nodes.push_back(memory);
    out.edges.push_back(edge(s,"edge:message-memory",InteractionEdgeKind::UsedMemoryView,message.node_id,memory.node_id));

    for(const auto&a:s.agent_templates){InteractionNode n;n.node_id="agent:"+a.invocation_id;n.kind=InteractionNodeKind::Agent;n.ref=ref;
        n.ref.agent_template_id=a.template_id;n.ref.agent_invocation_id=a.invocation_id;n.ref.child_agent_id=a.invocation_id;
        n.label=a.business_mode.empty()?a.template_id:a.business_mode;n.summary=a.completion_reason;
        n.display={{"template_id",a.template_id},{"template_revision",a.template_revision},{"hosting_mode",a.hosting_mode},
            {"plan_id",a.plan_id},{"plan_revision",a.plan_revision},{"session_id",a.session_id},{"completion_authority",a.completion_authority}};
        n.state=a.completion_reason.empty()?InteractionObjectState::Running:
            (a.completion_reason.find("open")!=std::string::npos?InteractionObjectState::Warning:InteractionObjectState::Passed);n.visibility=InteractionVisibility::User;
        n.source=source(s,a.invocation_id);n.updated_at=s.updated_at;out.nodes.push_back(n);
        out.edges.push_back(edge(s,"edge:plan-agent:"+a.invocation_id,InteractionEdgeKind::DelegatedTo,plan.node_id,n.node_id));}
    for(const auto&skill:s.skill_nodes){InteractionNode n;n.node_id="skill:"+skill.node_id;n.kind=InteractionNodeKind::SkillNode;n.ref=ref;
        n.ref.agent_invocation_id=s.agent_templates.empty()?"unassigned":s.agent_templates.front().invocation_id;n.ref.skill_node_id=skill.node_id;
        n.label=skill.role+" · "+skill.skill_id;n.summary=skill.state;n.display={{"skill_id",skill.skill_id},{"skill_version",skill.skill_version},
            {"runner",skill.runner},{"evidence_refs",skill.evidence_refs},{"artifact_refs",skill.artifact_refs}};
        n.state=(skill.state=="completed"||skill.state=="succeeded"||skill.state=="passed")?InteractionObjectState::Passed:
            (skill.state=="failed"?InteractionObjectState::Failed:InteractionObjectState::Running);n.visibility=InteractionVisibility::User;
        n.source=source(s,skill.node_id);n.updated_at=s.updated_at;out.nodes.push_back(n);
        if(!s.agent_templates.empty())out.edges.push_back(edge(s,"edge:agent-skill:"+skill.node_id,InteractionEdgeKind::ParentOf,
            "agent:"+s.agent_templates.front().invocation_id,n.node_id));
        for(const auto&id:skill.artifact_refs){InteractionNode artifact;artifact.node_id="artifact:"+id;artifact.kind=InteractionNodeKind::Artifact;
            artifact.ref=ref;artifact.ref.artifact_id=id;artifact.label="Produced artifact";artifact.summary=id;
            artifact.display={{"producer_skill_node",skill.node_id},{"output_digest",skill.output_digest}};
            artifact.state=InteractionObjectState::Passed;artifact.visibility=InteractionVisibility::User;
            artifact.source=source(s,id);artifact.updated_at=s.updated_at;out.nodes.push_back(artifact);
            out.edges.push_back(edge(s,"edge:skill-artifact:"+skill.node_id+":"+id,InteractionEdgeKind::Produced,n.node_id,artifact.node_id));}}

    for(const auto&h:s.hitl){InteractionNode n;n.node_id="approval:"+h.id;n.kind=InteractionNodeKind::Approval;n.ref=ref;n.ref.approval_id=h.id;
        n.label=h.kind;n.summary=h.summary;n.display={{"original_question",message.summary},{"source_message_id",c.user_message_id},
            {"requested_by",h.requested_by},{"deadline",h.deadline},{"allowed_actions",h.allowed_actions},{"resume_target",plan.node_id}};
        n.state=state(h.status);n.visibility=InteractionVisibility::User;n.source=source(s,h.id);n.updated_at=s.updated_at;out.nodes.push_back(n);
        out.edges.push_back(edge(s,"edge:message-approval:"+h.id,InteractionEdgeKind::RequestedApproval,message.node_id,n.node_id));
        out.edges.push_back(edge(s,"edge:approval-plan:"+h.id,InteractionEdgeKind::Resumes,n.node_id,plan.node_id));}
    for(const auto&e:s.evidence){InteractionNode n;n.node_id="evidence:"+e.id;n.kind=InteractionNodeKind::Evidence;n.ref=ref;n.ref.evidence_id=e.id;
        n.label=e.kind;n.summary=e.claim;n.display={{"source",e.source},{"authority",e.authority},{"freshness",e.freshness}};
        n.state=state(e.status);n.visibility=InteractionVisibility::User;n.source=source(s,e.id);n.updated_at=s.updated_at;out.nodes.push_back(n);
        out.edges.push_back(edge(s,"edge:plan-evidence:"+e.id,InteractionEdgeKind::VerifiedBy,plan.node_id,n.node_id));}
    for(const auto&a:s.assurance){for(const auto&id:a.finding_ids){InteractionNode finding;finding.node_id="finding:"+id;
        finding.kind=InteractionNodeKind::Finding;finding.ref=ref;finding.ref.finding_id=id;finding.label=a.label+" finding";
        finding.summary="Oracle "+a.oracle+" · verifier "+a.verifier;finding.display={{"assurance_layer",a.id},{"oracle",a.oracle},{"verifier",a.verifier}};
        finding.state=state(a.status);finding.visibility=InteractionVisibility::User;finding.source=source(s,id);finding.updated_at=s.updated_at;
        out.nodes.push_back(finding);out.edges.push_back(edge(s,"edge:verify:"+a.id+":"+id,InteractionEdgeKind::VerifiedBy,plan.node_id,finding.node_id));}}
    for(const auto&n:out.nodes)out.source_revisions.push_back(n.source);
    out.digest=encode(out).at("digest");return out;
}
} // namespace agent_framework::ui
