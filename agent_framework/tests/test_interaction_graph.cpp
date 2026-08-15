#include <cassert>
#include <iostream>

#include "agent/ui/interaction_graph.hpp"

using namespace agent_framework::ui;

namespace {
InteractionRef message_ref() {
    InteractionRef r; r.tenant_id="tenant-a"; r.conversation_id="conversation-a";
    r.turn_id="turn-1"; r.message_id="message-1"; return r;
}
InteractionSourceRevision source() { return {"conversation_store","message-1",1,"sha256:source"}; }
InteractionNode message_node() {
    InteractionNode n; n.node_id="message:message-1"; n.kind=InteractionNodeKind::Message;
    n.ref=message_ref(); n.label="User request"; n.summary="Implement the approved work";
    n.display={{"role","user"},{"text","Implement the approved work"}};
    n.state=InteractionObjectState::Passed; n.visibility=InteractionVisibility::User;
    n.source=source(); n.updated_at="2026-08-15T00:00:00Z"; return n;
}
}

int main() {
    auto node=message_node(); assert(validate(node).empty());
    auto json=encode(node); std::vector<agent_framework::contracts::ContractIssue> issues;
    auto decoded=decode_interaction_node(json,&issues); assert(decoded && issues.empty());
    assert(decoded->display.at("role")=="user");

    auto unknown=json; unknown["unexpected"]=true;
    unknown["digest"]=agent_framework::contracts::canonical_digest([&]{auto v=unknown;v.erase("digest");return v;}()).value();
    issues.clear(); assert(!decode_interaction_node(unknown,&issues)); assert(!issues.empty());

    auto tampered=json; tampered["summary"]="tampered";
    assert(!decode_interaction_node(tampered));

    node.display={{"chain_of_thought","private reasoning"}};
    assert(!validate(node).empty());

    InteractionNode plan=node; plan.node_id="plan:p1"; plan.kind=InteractionNodeKind::Plan;
    plan.ref=message_ref(); plan.ref.plan_id="p1"; plan.ref.plan_revision=1;
    plan.display={{"facts",nlohmann::json::array({"fact"})},{"unknowns",nlohmann::json::array({"unknown"})}};
    assert(validate(plan).empty());
    plan.ref.plan_revision=0; assert(!validate(plan).empty());

    UiInteractionEvent event; event.event_id="uie-1"; event.tenant_id="tenant-a";
    event.conversation_id="conversation-a"; event.sequence=1; event.event_type="message.selected";
    event.visibility=InteractionVisibility::User; event.primary_ref=message_ref();
    event.display={{"label","User request"}}; event.navigation_target={"conversation","message:message-1",1};
    event.source=source(); event.timestamp="2026-08-15T00:00:00Z";
    assert(validate(event).empty());
    auto cross=message_ref(); cross.tenant_id="tenant-b"; event.related_refs.push_back(cross);
    assert(!validate(event).empty());
    std::cout << "interaction graph contracts passed\n";
}
