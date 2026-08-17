#include "agent/ui/runtime_event_projector.hpp"

#include <algorithm>

namespace agent_framework::ui {
namespace {

InteractionVisibility visibility(conversation::EventVisibility value) {
    if(value==conversation::EventVisibility::Audit)return InteractionVisibility::Audit;
    if(value==conversation::EventVisibility::Operations||value==conversation::EventVisibility::Internal)
        return InteractionVisibility::Operations;
    return InteractionVisibility::User;
}

InteractionObjectState state(const nlohmann::json& payload) {
    const auto value=payload.value("state",std::string{});
    if(value=="completed"||value=="passed"||value=="answered"||value=="closed")return InteractionObjectState::Passed;
    if(value=="failed"||value=="error")return InteractionObjectState::Failed;
    if(value=="waiting"||value=="awaiting_input"||value=="pending")return InteractionObjectState::Waiting;
    if(value=="blocked")return InteractionObjectState::Blocked;
    if(value=="warning")return InteractionObjectState::Warning;
    if(value=="superseded")return InteractionObjectState::Superseded;
    return InteractionObjectState::Running;
}

InteractionNodeKind kind(const conversation::RuntimeEventEnvelope& event) {
    const auto& type=event.event_type;const auto& payload=event.payload;
    if(type.find("decision")!=std::string::npos)return InteractionNodeKind::Decision;
    if(type.find("llm_invocation")!=std::string::npos&&payload.contains("invocation_id"))
        return InteractionNodeKind::Agent;
    if(type.find("plan")!=std::string::npos&&payload.contains("plan_id")&&
       payload.value("plan_revision",0ULL)>0)return InteractionNodeKind::Plan;
    if(type.find("semantic")!=std::string::npos||type.find("planning")!=std::string::npos)
        return InteractionNodeKind::Understanding;
    if(type.find("tool")!=std::string::npos&&(event.tool_call_id||payload.contains("invocation_id")))
        return InteractionNodeKind::ToolInvocation;
    if(type.find("memory")!=std::string::npos&&payload.contains("memory_snapshot_id")&&payload.contains("memory_view_digest"))
        return InteractionNodeKind::MemoryView;
    if(type.find("approval")!=std::string::npos&&payload.contains("approval_id"))return InteractionNodeKind::Approval;
    if(type.find("artifact")!=std::string::npos&&payload.contains("artifact_id"))return InteractionNodeKind::Artifact;
    if(type.find("evidence")!=std::string::npos&&payload.contains("evidence_id"))return InteractionNodeKind::Evidence;
    if(type.find("closure")!=std::string::npos&&!event.run_id.empty()&&payload.contains("task_id"))return InteractionNodeKind::Closure;
    return InteractionNodeKind::CognitionStage;
}

nlohmann::json safe_display(const conversation::RuntimeEventEnvelope& event) {
    nlohmann::json out=nlohmann::json::object();
    for(const auto* key:{"state","summary","reason","work_shape","effect_class",
                         "assurance_tier","planning_depth","progress","tool",
                         "run_revision","lease_epoch","role","provider","model",
                         "deployment_revision","prompt_revision","schema_version",
                         "latency_ms","confidence","fallback","outcome","error_code"})
        if(event.payload.contains(key)&&(event.payload.at(key).is_string()||
           event.payload.at(key).is_number()||event.payload.at(key).is_boolean()))
            out[key]=event.payload.at(key);
    out["runtime_sequence"]=event.sequence;
    return out;
}

InteractionRef ref(const conversation::RuntimeEventEnvelope& event,InteractionNodeKind node_kind) {
    InteractionRef out;out.tenant_id=event.tenant_id;out.conversation_id=event.conversation_id;
    out.turn_id=event.turn_id;out.run_id=event.run_id;
    out.task_id=event.payload.value("task_id",std::string{});
    out.decision_id=event.payload.value("decision_id",std::string{});
    if(node_kind==InteractionNodeKind::Agent)
        out.agent_invocation_id=event.payload.value("invocation_id",std::string{});
    if(node_kind==InteractionNodeKind::Plan){out.plan_id=event.payload.value("plan_id",std::string{});out.plan_revision=event.payload.value("plan_revision",0ULL);}
    if(node_kind==InteractionNodeKind::ToolInvocation)out.tool_invocation_id=event.tool_call_id.value_or(event.payload.value("invocation_id",std::string{}));
    if(node_kind==InteractionNodeKind::MemoryView){out.memory_snapshot_id=event.payload.value("memory_snapshot_id",std::string{});out.memory_view_digest=event.payload.value("memory_view_digest",std::string{});}
    if(node_kind==InteractionNodeKind::Approval)out.approval_id=event.payload.value("approval_id",std::string{});
    if(node_kind==InteractionNodeKind::Artifact)out.artifact_id=event.payload.value("artifact_id",std::string{});
    if(node_kind==InteractionNodeKind::Evidence)out.evidence_id=event.payload.value("evidence_id",std::string{});
    return out;
}
}

RuntimeProjectionResult RuntimeEventInteractionProjector::synchronize(
    const conversation::ConversationIdentity& identity,std::size_t batch_size) {
    RuntimeProjectionResult result;
    if(identity.tenant_id.empty()||identity.conversation_id.empty()||batch_size==0) {
        result.error="runtime_projection_contract_invalid";return result;
    }
    result.runtime_head=source_.last_event_sequence(identity);
    auto snapshot=target_.snapshot(identity.tenant_id,identity.conversation_id,
                                   InteractionVisibility::Audit);
    auto cursor=snapshot?snapshot->head_sequence:0;
    auto revision=snapshot?snapshot->revision:0;
    if(cursor>result.runtime_head){result.error="projection_cursor_ahead_of_runtime";return result;}
    const auto floor=source_.event_retention_floor(identity);
    if(floor>0&&cursor+1<floor){result.error="runtime_projection_cursor_expired";return result;}
    while(cursor<result.runtime_head) {
        const auto events=source_.events(identity,cursor,batch_size);
        if(events.empty()){
            const auto read_error=source_.event_read_error(identity);
            result.error=read_error.empty()?"runtime_projection_event_gap":
                "runtime_projection_source_"+read_error;
            return result;
        }
        for(const auto& runtime:events) {
            if(runtime.sequence!=cursor+1){result.error="runtime_projection_event_gap";return result;}
            const auto node_kind=kind(runtime);const auto source_digest=runtime.digest.empty()?
                contracts::canonical_digest(conversation::encode(runtime)).value_or(""):runtime.digest;
            InteractionSourceRevision source{"conversation_events",runtime.event_id,runtime.sequence,source_digest};
            InteractionNode node;node.node_id="runtime-event:"+std::to_string(runtime.sequence);
            node.kind=node_kind;node.ref=ref(runtime,node_kind);node.revision=revision+1;
            node.label=runtime.event_type;node.summary=runtime.payload.value("summary",runtime.payload.value("reason",std::string{}));
            node.display=safe_display(runtime);node.state=state(runtime.payload);
            node.visibility=visibility(runtime.visibility);node.source=source;node.updated_at=runtime.timestamp;
            UiInteractionEvent event;event.event_id="runtime-projection:"+runtime.event_id;
            event.tenant_id=runtime.tenant_id;event.conversation_id=runtime.conversation_id;
            event.sequence=runtime.sequence;event.event_type=runtime.event_type;
            event.visibility=node.visibility;event.primary_ref=node.ref;event.display=node.display;
            event.navigation_target={std::string(name(node_kind)),node.node_id,revision+1};
            event.source=source;event.timestamp=runtime.timestamp;
            auto committed=target_.commit({event,{node},{}},revision);
            if(!committed.ok()){result.error=committed.error;result.projection_head=committed.head_sequence;
                result.projection_revision=committed.revision;return result;}
            cursor=committed.head_sequence;revision=committed.revision;++result.applied;
        }
    }
    result.ok=true;result.projection_head=cursor;result.projection_revision=revision;return result;
}

} // namespace agent_framework::ui
