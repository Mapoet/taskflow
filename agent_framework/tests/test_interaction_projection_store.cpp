#include <cassert>
#include <filesystem>
#include <iostream>

#include "agent/ui/interaction_projection_store.hpp"

using namespace agent_framework::ui;

namespace {
InteractionSourceRevision src(std::string id) { return {"test_store",std::move(id),1,"sha256:source"}; }
InteractionRef base_ref() { InteractionRef r; r.tenant_id="t"; r.conversation_id="c"; r.turn_id="turn"; return r; }
InteractionNode message() { InteractionNode n; n.node_id="message:m"; n.kind=InteractionNodeKind::Message;
    n.ref=base_ref(); n.ref.message_id="m"; n.label="request"; n.summary="request"; n.display={{"role","user"}};
    n.state=InteractionObjectState::Passed; n.visibility=InteractionVisibility::User; n.source=src("m"); n.updated_at="2026-08-15T00:00:00Z"; return n; }
UiInteractionEvent event(std::uint64_t sequence,std::string id,std::string object) { UiInteractionEvent e;
    e.event_id=std::move(id); e.tenant_id="t"; e.conversation_id="c"; e.sequence=sequence;
    e.event_type="projection.updated"; e.visibility=InteractionVisibility::User; e.primary_ref=base_ref(); e.primary_ref.message_id="m";
    e.display={{"label","updated"}}; e.navigation_target={"conversation",std::move(object),1};
    e.source=src(e.event_id); e.timestamp="2026-08-15T00:00:00Z"; return e; }
InteractionSnapshot projection(std::string conversation, std::string node_id) {
    InteractionSnapshot value; value.tenant_id="t"; value.conversation_id=std::move(conversation);
    value.updated_at="2026-08-15T00:00:02Z"; value.digest="sha256:projection";
    auto node=message();node.node_id=std::move(node_id);node.ref.conversation_id=value.conversation_id;
    node.ref.message_id=node.node_id;value.nodes.push_back(std::move(node));return value;
}
class ConflictOnceStore final : public InteractionProjectionStore {
public:
    explicit ConflictOnceStore(InteractionProjectionStore& inner):inner_(inner){}
    InteractionCommitResult commit(const InteractionCommit& value,std::uint64_t expected) override {
        if(first_){first_=false;auto competing=value;competing.event.event_id += ":competitor";
            competing.event.source.object_id=competing.event.event_id;
            const auto won=inner_.commit(competing,expected);assert(won.ok());}
        return inner_.commit(value,expected);
    }
    std::optional<InteractionSnapshot> snapshot(std::string_view t,std::string_view c,InteractionVisibility v) override{return inner_.snapshot(t,c,v);}
    std::vector<UiInteractionEvent> events(std::string_view t,std::string_view c,std::uint64_t a,std::size_t l,InteractionVisibility v) override{return inner_.events(t,c,a,l,v);}
    std::optional<InteractionNode> node(std::string_view t,std::string_view c,std::string_view n,InteractionVisibility v) override{return inner_.node(t,c,n,v);}
private: InteractionProjectionStore& inner_;bool first_{true};
};
}

int main() {
    const auto path=(std::filesystem::temp_directory_path()/"af-interaction-projection-test.sqlite").string();
    std::filesystem::remove(path);
    {
        SQLiteInteractionProjectionStore store(path);
        InteractionEdge edge; edge.edge_id="edge:message-plan"; edge.kind=InteractionEdgeKind::PlannedBy;
        edge.from_node_id="message:m"; edge.to_node_id="plan:p"; edge.visibility=InteractionVisibility::User;
        edge.source=src("edge"); edge.updated_at="2026-08-15T00:00:00Z";
        auto first=store.commit({event(1,"e1","message:m"),{message()},{edge}},0);
        assert(first.status==InteractionCommitStatus::Committed && first.revision==1);
        auto snap=store.snapshot("t","c",InteractionVisibility::User); assert(snap);
        assert(snap->nodes.size()==1 && snap->orphan_edge_ids.size()==1);

        InteractionNode plan; plan.node_id="plan:p"; plan.kind=InteractionNodeKind::Plan; plan.ref=base_ref();
        plan.ref.plan_id="p"; plan.ref.plan_revision=1; plan.label="Plan"; plan.summary="Executable plan";
        plan.display={{"steps",nlohmann::json::array({"inspect","implement","verify"})}};
        plan.state=InteractionObjectState::Running; plan.visibility=InteractionVisibility::Operations;
        plan.source=src("p"); plan.updated_at="2026-08-15T00:00:01Z";
        auto second=store.commit({event(2,"e2","plan:p"),{plan},{}},1);
        assert(second.status==InteractionCommitStatus::Committed && second.revision==2);
        auto user=store.snapshot("t","c",InteractionVisibility::User); assert(user && user->nodes.size()==1);
        auto ops=store.snapshot("t","c",InteractionVisibility::Operations); assert(ops && ops->nodes.size()==2);
        assert(ops->orphan_edge_ids.empty());
        auto replay=store.events("t","c",0,10,InteractionVisibility::User); assert(replay.size()==2);
        auto stale=store.commit({event(3,"e3","plan:p"),{},{}},0);
        assert(stale.status==InteractionCommitStatus::RevisionConflict);

        auto default_commit=commit_interaction_projection(store,projection("default","message:default"));
        auto real_commit=commit_interaction_projection(store,projection("conversation-real","message:real"));
        assert(default_commit.ok()&&default_commit.revision==1);
        assert(real_commit.ok()&&real_commit.revision==1); // independent stream, not default revision
        ConflictOnceStore racing(store);
        auto retried=commit_interaction_projection(racing,projection("conversation-race","message:race"),2);
        assert(retried.ok()&&retried.revision==2&&retried.head_sequence==2);
    }
    {
        SQLiteInteractionProjectionStore reopened(path);
        auto snap=reopened.snapshot("t","c",InteractionVisibility::Operations);
        assert(snap && snap->revision==2 && snap->head_sequence==2 && snap->orphan_edge_ids.empty());
        auto found=reopened.node("t","c","plan:p",InteractionVisibility::Operations); assert(found);
        assert(!reopened.node("t","c","plan:p",InteractionVisibility::User));
    }
    std::filesystem::remove(path);
    std::cout << "interaction projection durability passed\n";
}
