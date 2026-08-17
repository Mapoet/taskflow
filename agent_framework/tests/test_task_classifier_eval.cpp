#include <cassert>
#include <fstream>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

int main(){
    std::ifstream input(AGENT_TASK_CLASSIFIER_CORPUS);
    assert(input.good());std::string line;std::size_t count=0,negative=0;
    std::set<std::string> ids,intents,profiles;
    while(std::getline(input,line)){
        if(line.empty())continue;auto row=nlohmann::json::parse(line);++count;
        assert(row.size()==7);assert(row.at("id").is_string());
        assert(ids.insert(row.at("id").get<std::string>()).second);
        assert(row.at("input").is_string()&&!row.at("input").get<std::string>().empty());
        assert(row.at("active").is_boolean()&&row.at("high_side_effect").is_boolean());
        intents.insert(row.at("intent").get<std::string>());
        profiles.insert(row.at("profile").get<std::string>());
        static const std::set<std::string> effects{"none","read_only","workspace_write","external","destructive"};
        assert(effects.count(row.at("effect"))==1);
        if(!row.at("high_side_effect").get<bool>() &&
           row.at("intent").get<std::string>()!="profile_confirmation"){
            ++negative;const auto profile=row.at("profile").get<std::string>();
            assert(profile!="external_action"&&profile!="code_change"&&profile!="artifact_delivery");
        }
    }
    assert(count>=30&&negative>=15);assert(intents.size()>=8);assert(profiles.size()==6);
}
