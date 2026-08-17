#include <cassert>
#include "agent/conversation/task_routing_policy.hpp"

using namespace agent_framework::conversation;

int main() {
    TaskClassification chat;
    auto route=decide_task_route(chat);
    assert(!route.promote_to_task&&!route.planning_required);

    TaskClassification research;
    research.work_shape=WorkShape::LongRunningTask;
    research.effect_class=EffectClass::ReadOnly;
    route=decide_task_route(research);
    assert(route.promote_to_task&&route.planning_required);
    assert(route.planning_depth==PlanningDepth::Comprehensive);

    TaskClassification edit;
    edit.work_shape=WorkShape::BoundedTask;
    edit.effect_class=EffectClass::WorkspaceWrite;
    edit.assurance_tier=AssuranceTier::Functional;
    route=decide_task_route(edit);
    assert(route.promotion==TaskPromotionMode::BoundedTask);
    assert(route.planning_depth==PlanningDepth::Bounded);

    TaskClassification certification;
    certification.assurance_tier=AssuranceTier::ProductionCertification;
    route=decide_task_route(certification);
    assert(route.promote_to_task&&route.planning_required);
    assert(route.planning_depth==PlanningDepth::Comprehensive);

    route=decide_task_route(chat,true);
    assert(route.promote_to_task&&route.planning_required);
}
