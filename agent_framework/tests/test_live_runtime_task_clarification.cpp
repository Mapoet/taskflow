#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>

#include "../examples/common/agent_example_bootstrap.hpp"

namespace {
class WeakClassifier final : public agent_framework::conversation::TaskClassifier {
public:
    agent_framework::conversation::TaskClassification classify(
        std::string_view,bool) override {
        agent_framework::conversation::TaskClassification value;
        value.profile=agent_framework::conversation::TaskExecutionProfile::CodeChange;
        value.effect_class=agent_framework::conversation::EffectClass::WorkspaceWrite;
        value.confidence=.5;value.requires_confirmation=true;
        value.clarification=agent_framework::conversation::TaskClarificationProposal{
            "Do you want an explanation or an implementation?",{
                {"explain","Explain only","Do not edit files",
                 agent_framework::conversation::TaskExecutionProfile::ReadOnlyAnalysis},
                {"implement","Implement and test","Edit the parser and run tests",
                 agent_framework::conversation::TaskExecutionProfile::CodeChange}}};
        value.decision_id="live-runtime-decision";return value;
    }
};
}

int main(){
    using namespace agent_framework;
    const auto path=(std::filesystem::temp_directory_path()/"agent-live-runtime-clarification.sqlite").string();
    std::filesystem::remove(path);::setenv("AGENT_CONVERSATION_DB",path.c_str(),1);
    ::setenv("AGENT_TENANT_ID","tenant",1);::setenv("AGENT_CONVERSATION_ID","conversation",1);
    example::LiveRuntime runtime;runtime.explicit_legacy_fallback=true;
    runtime.task_classifier=std::make_shared<WeakClassifier>();
    int calls=0;
    runtime.long_task_executor=[&calls](const conversation::HarnessSupportedTurnRequest&){
        ++calls;conversation::ModelTurnOutcome outcome;
        outcome.reason=conversation::ModelTurnStopReason::EndTurn;
        outcome.candidate_answer="completed";return outcome;};
    auto graph=[](){WorkflowResult result{};result.success=true;
        result.outputs={{"final_answer","completed"}};return result;};
    auto waiting=example::run_conversation_turn(runtime,"clarification-demo","modify parser",graph);
    assert(waiting.error.empty()&&waiting.checkpoint.phase==conversation::TurnPhase::AwaitingInput);
    assert(calls==0);
    auto resumed=example::run_conversation_turn(runtime,"clarification-demo","implement",graph);
    if(!resumed.error.empty()||resumed.checkpoint.phase!=conversation::TurnPhase::Completed)
        std::cerr<<"resume error="<<resumed.error<<" phase="
                 <<conversation::name(resumed.checkpoint.phase)<<" calls="<<calls<<'\n';
    assert(resumed.error.empty()&&resumed.checkpoint.phase==conversation::TurnPhase::Completed);
    assert(calls==1);
    auto replay=example::run_conversation_turn(runtime,"clarification-demo","implement",graph);
    assert(replay.error.empty()&&replay.checkpoint.turn_id==resumed.checkpoint.turn_id&&
           replay.checkpoint.phase==conversation::TurnPhase::Completed&&calls==1);
    conversation::SQLiteTaskRegistry tasks(path);auto active=tasks.active({"tenant","conversation"});
    assert(active&&tasks.requirements(active->identity,active->task_id).size()==1);
    ::unsetenv("AGENT_CONVERSATION_DB");::unsetenv("AGENT_TENANT_ID");
    ::unsetenv("AGENT_CONVERSATION_ID");std::filesystem::remove(path);
}
