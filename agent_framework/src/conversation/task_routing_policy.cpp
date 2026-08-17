#include "agent/conversation/task_routing_policy.hpp"

namespace agent_framework::conversation {

std::string_view name(TaskPromotionMode value) {
    switch(value) {
        case TaskPromotionMode::DirectTurn:return "direct_turn";
        case TaskPromotionMode::BoundedTask:return "bounded_task";
        case TaskPromotionMode::LongRunningTask:return "long_running_task";
        case TaskPromotionMode::ContinuousTask:return "continuous_task";
    }
    return "direct_turn";
}

std::string_view name(PlanningDepth value) {
    switch(value) {
        case PlanningDepth::None:return "none";
        case PlanningDepth::Bounded:return "bounded";
        case PlanningDepth::Comprehensive:return "comprehensive";
        case PlanningDepth::Continuous:return "continuous";
    }
    return "none";
}

TaskRoutingDecision decide_task_route(const TaskClassification& semantic,
    bool user_requested_plan,std::size_t dependencies) {
    TaskRoutingDecision out;
    switch(semantic.work_shape) {
        case WorkShape::SingleTurn:break;
        case WorkShape::BoundedTask:
            out.promotion=TaskPromotionMode::BoundedTask;out.promote_to_task=true;
            out.reasons.push_back("bounded_work_shape");break;
        case WorkShape::LongRunningTask:
            out.promotion=TaskPromotionMode::LongRunningTask;out.promote_to_task=true;
            out.planning_required=true;out.planning_depth=PlanningDepth::Comprehensive;
            out.reasons.push_back("long_running_work_shape");break;
        case WorkShape::ContinuousTask:
            out.promotion=TaskPromotionMode::ContinuousTask;out.promote_to_task=true;
            out.planning_required=true;out.planning_depth=PlanningDepth::Continuous;
            out.reasons.push_back("continuous_work_shape");break;
    }
    if(semantic.effect_class==EffectClass::WorkspaceWrite||
       semantic.effect_class==EffectClass::External||
       semantic.effect_class==EffectClass::Destructive) {
        if(!out.promote_to_task) {
            out.promote_to_task=true;out.promotion=TaskPromotionMode::BoundedTask;
        }
        out.planning_required=true;
        if(out.planning_depth==PlanningDepth::None)
            out.planning_depth=PlanningDepth::Bounded;
        out.reasons.push_back("effect_requires_verifiable_execution");
    }
    if(semantic.assurance_tier==AssuranceTier::Professional||
       semantic.assurance_tier==AssuranceTier::ProductionCertification) {
        if(!out.promote_to_task) {
            out.promote_to_task=true;out.promotion=TaskPromotionMode::BoundedTask;
        }
        out.planning_required=true;
        if(out.planning_depth==PlanningDepth::None||
           out.planning_depth==PlanningDepth::Bounded)
            out.planning_depth=PlanningDepth::Comprehensive;
        out.reasons.push_back("assurance_requires_plan_and_evidence");
    } else if(semantic.assurance_tier==AssuranceTier::Functional&&out.promote_to_task) {
        out.planning_required=true;
        if(out.planning_depth==PlanningDepth::None)
            out.planning_depth=PlanningDepth::Bounded;
        out.reasons.push_back("functional_assurance_requires_acceptance_plan");
    }
    if(dependencies>1) {
        out.promote_to_task=true;
        if(out.promotion==TaskPromotionMode::DirectTurn)
            out.promotion=TaskPromotionMode::BoundedTask;
        out.planning_required=true;
        if(out.planning_depth==PlanningDepth::None)
            out.planning_depth=dependencies>4?PlanningDepth::Comprehensive:PlanningDepth::Bounded;
        out.reasons.push_back("multiple_dependencies");
    }
    if(user_requested_plan) {
        out.promote_to_task=true;
        if(out.promotion==TaskPromotionMode::DirectTurn)
            out.promotion=TaskPromotionMode::BoundedTask;
        out.planning_required=true;
        if(out.planning_depth==PlanningDepth::None)out.planning_depth=PlanningDepth::Bounded;
        out.reasons.push_back("user_requested_plan");
    }
    if(out.reasons.empty())out.reasons.push_back("direct_single_turn");
    return out;
}

}  // namespace agent_framework::conversation
