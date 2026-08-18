#include <cassert>
#include <fstream>
#include <filesystem>
#include <set>
#include <nlohmann/json.hpp>
#include "agent/conversation/task_routing_policy.hpp"

using namespace agent_framework::conversation;
int main() {
    std::ifstream input(AGENT_TASK_SEMANTICS_CORPUS);assert(input.good());
    std::string line;std::set<std::string> ids;std::size_t count=0,planned=0,direct=0;
    TaskSemanticCalibrationMetrics metrics;
    while(std::getline(input,line)) {if(line.empty())continue;auto row=nlohmann::json::parse(line);
        assert(row.size()==6&&ids.insert(row.at("id").get<std::string>()).second);++count;
        TaskClassification semantic;semantic.work_shape=*work_shape(row.at("work_shape").get<std::string>());
        semantic.assurance_tier=*assurance_tier(row.at("assurance").get<std::string>());
        const auto effect=row.at("effect").get<std::string>();
        for(auto candidate:{EffectClass::None,EffectClass::ReadOnly,EffectClass::WorkspaceWrite,
            EffectClass::External,EffectClass::Destructive})if(name(candidate)==effect)semantic.effect_class=candidate;
        const auto route=decide_task_route(semantic);
        assert(route.planning_required==row.at("planning").get<bool>());
        metrics.observe(semantic,{semantic.effect_class,row.at("planning").get<bool>(),
            route.planning_required,false,false});
        route.planning_required?++planned:++direct;
    }
    assert(count>=18&&planned>=12&&direct>=4);
    auto report=metrics.snapshot();
    report["campaign_schema"]="agent.task_semantics_campaign/v1";
    report["evidence_level"]="offline";
    report["corpus"]="task-semantics-v4";
    report["policy_revision"]="task-promotion-planning-v1";
    report["gates"]={{"false_high_effect_max",0},{"missed_planning_max",0},
        {"unnecessary_planning_max",0},{"unnecessary_clarification_max",0}};
    report["passed"]=report.at("false_high_effect").at("count")==0&&
        report.at("missed_planning").at("count")==0&&
        report.at("unnecessary_planning").at("count")==0&&
        report.at("unnecessary_clarification").at("count")==0;
    assert(report.at("passed").get<bool>());
    const std::filesystem::path output=AGENT_TASK_SEMANTICS_REPORT;
    std::filesystem::create_directories(output.parent_path());
    std::ofstream evidence(output,std::ios::binary|std::ios::trunc);assert(evidence.good());
    evidence<<report.dump(2)<<'\n';evidence.close();assert(evidence.good());
}
