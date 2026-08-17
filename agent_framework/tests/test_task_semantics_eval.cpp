#include <cassert>
#include <fstream>
#include <set>
#include <nlohmann/json.hpp>
#include "agent/conversation/task_routing_policy.hpp"

using namespace agent_framework::conversation;
int main() {
    std::ifstream input(AGENT_TASK_SEMANTICS_CORPUS);assert(input.good());
    std::string line;std::set<std::string> ids;std::size_t count=0,planned=0,direct=0;
    while(std::getline(input,line)) {if(line.empty())continue;auto row=nlohmann::json::parse(line);
        assert(row.size()==6&&ids.insert(row.at("id").get<std::string>()).second);++count;
        TaskClassification semantic;semantic.work_shape=*work_shape(row.at("work_shape").get<std::string>());
        semantic.assurance_tier=*assurance_tier(row.at("assurance").get<std::string>());
        const auto effect=row.at("effect").get<std::string>();
        for(auto candidate:{EffectClass::None,EffectClass::ReadOnly,EffectClass::WorkspaceWrite,
            EffectClass::External,EffectClass::Destructive})if(name(candidate)==effect)semantic.effect_class=candidate;
        const auto route=decide_task_route(semantic);
        assert(route.planning_required==row.at("planning").get<bool>());
        route.planning_required?++planned:++direct;
    }
    assert(count>=18&&planned>=12&&direct>=4);
}
