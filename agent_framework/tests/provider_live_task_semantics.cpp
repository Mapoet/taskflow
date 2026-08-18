#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>

#include <nlohmann/json.hpp>

#include "agent/conversation/task_classifier.hpp"
#include "agent/conversation/task_routing_policy.hpp"
#include "agent/llm_client/llm_client.hpp"

using namespace agent_framework;
using namespace agent_framework::conversation;

namespace {
std::string env(std::string_view key) {
    const auto* value=std::getenv(std::string(key).c_str());
    return value&&*value?value:"";
}
EffectClass effect(std::string_view value) {
    for(auto candidate:{EffectClass::None,EffectClass::ReadOnly,EffectClass::WorkspaceWrite,
                        EffectClass::External,EffectClass::Destructive})
        if(name(candidate)==value)return candidate;
    throw std::invalid_argument("unknown expected effect");
}
}

int main(int argc,char** argv) {
    if(argc!=3) {
        std::cerr<<"usage: provider_live_task_semantics CORPUS REPORT\n";
        return 64;
    }
    const auto provider=env("AGENT_LLM_PROVIDER");
    const auto model=env("AGENT_LLM_MODEL");
    if(provider.empty()||model.empty()||
       (env("OPENAI_API_KEY").empty()&&env("ANTHROPIC_API_KEY").empty())) {
        std::cerr<<"ProviderLive configuration incomplete\n";
        return 78;
    }
    auto client=std::make_shared<LLMClient>(LLMClient::from_env());
    ModelConfig config;config.model_name=model;config.stream=false;
    config.http_timeout_sec=std::max(30,std::atoi(env("AGENT_HTTP_TIMEOUT_SEC").c_str()));
    client->configure(provider,config);
    LLMTaskClassifier classifier(client,provider);
    TaskSemanticCalibrationMetrics metrics;
    std::ifstream corpus(argv[1]);
    if(!corpus)return 66;
    std::string line;std::size_t samples=0,failures=0;
    nlohmann::json failure_ids=nlohmann::json::array();
    while(std::getline(corpus,line)) {
        if(line.empty())continue;
        const auto row=nlohmann::json::parse(line);++samples;
        const auto actual=classifier.classify(row.at("input").get<std::string>(),false);
        if(!actual) {++failures;failure_ids.push_back(row.at("id"));continue;}
        const auto route=decide_task_route(actual);
        metrics.observe(actual,{effect(row.at("effect").get<std::string>()),
            row.at("planning").get<bool>(),route.planning_required,false,false});
    }
    auto report=metrics.snapshot();
    report["campaign_schema"]="agent.task_semantics_campaign/v1";
    report["evidence_level"]="provider_live";
    report["provider"]=provider;
    report["model"]=model;
    report["samples_requested"]=samples;
    report["provider_failures"]=failures;
    report["failure_case_ids"]=std::move(failure_ids);
    report["passed"]=failures==0&&report.at("false_high_effect").at("count")==0&&
        report.at("missed_planning").at("count")==0;
    const std::filesystem::path output=argv[2];
    std::filesystem::create_directories(output.parent_path());
    std::ofstream evidence(output,std::ios::binary|std::ios::trunc);
    if(!evidence)return 73;
    evidence<<report.dump(2)<<'\n';
    return report.at("passed").get<bool>()?0:1;
}
