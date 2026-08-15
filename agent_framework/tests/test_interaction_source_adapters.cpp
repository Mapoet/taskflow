#include <cassert>
#include <iostream>
#include <set>

#include "agent/ui/interaction_source_adapters.hpp"

using namespace agent_framework;
using namespace agent_framework::ui;

int main(){
    auto ops=Phase4OperationsProjection::demo_snapshot();
    auto snapshot=project_interactions(ops,{"conversation-1","turn-1","message-1","Original scientific request"});
    std::set<InteractionNodeKind> kinds;for(const auto&node:snapshot.nodes){kinds.insert(node.kind);assert(validate(node).empty());}
    for(auto required:{InteractionNodeKind::Message,InteractionNodeKind::Thinking,InteractionNodeKind::Plan,
        InteractionNodeKind::PlanNode,InteractionNodeKind::MemoryView,InteractionNodeKind::Agent,
        InteractionNodeKind::SkillNode,InteractionNodeKind::Approval,InteractionNodeKind::Evidence,
        InteractionNodeKind::Finding,InteractionNodeKind::Artifact})assert(kinds.count(required));
    std::set<std::string> node_ids;for(const auto&node:snapshot.nodes)assert(node_ids.insert(node.node_id).second);
    for(const auto&edge:snapshot.edges){assert(validate(edge).empty());assert(node_ids.count(edge.from_node_id));assert(node_ids.count(edge.to_node_id));}
    auto approval=std::find_if(snapshot.nodes.begin(),snapshot.nodes.end(),[](const auto&n){return n.kind==InteractionNodeKind::Approval;});
    assert(approval!=snapshot.nodes.end());assert(approval->display.at("original_question")=="Original scientific request");
    auto memory=std::find_if(snapshot.nodes.begin(),snapshot.nodes.end(),[](const auto&n){return n.kind==InteractionNodeKind::MemoryView;});
    assert(memory!=snapshot.nodes.end() && memory->display.at("layers").is_array());
    assert(snapshot.digest==encode(snapshot).at("digest"));
    std::cout<<"interaction source adapters passed\n";
}
