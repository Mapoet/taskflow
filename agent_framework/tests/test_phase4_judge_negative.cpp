#include <algorithm>
#include <cassert>

#include "phase4_judge_test_support.hpp"

int main(){using namespace phase4_judge_test;
    {auto s=suite("task-f6e-leak");DatasetRegistry data;populate(data,s);auto b=run(s,false),c=run(s,true);c.cases[0].artifact["model_revision"]="secret";memory_v2::MemoryProviderRegistry p;memory_v2::MemoryViewEngine views(p);InMemoryJudgeStore store;ScriptedJudge model;LLMJudgeWorkflow w(views,store,model);auto r=w.run(s,data,b,c,subject(s.metadata),options("leak"));assert(r.state==JudgeWorkflowState::Failed&&r.error_code=="judge_input_invalid"&&model.invocations==0);}
    {auto s=suite("task-f6e-schema");DatasetRegistry data;populate(data,s);auto b=run(s,false),c=run(s,true);memory_v2::MemoryProviderRegistry p;memory_v2::MemoryViewEngine views(p);InMemoryJudgeStore store;ScriptedJudge model;model.invalid_alias=true;LLMJudgeWorkflow w(views,store,model);auto r=w.run(s,data,b,c,subject(s.metadata),options("schema"));assert(r.state==JudgeWorkflowState::ManualReview&&r.error_code=="primary_verdict_invalid"&&model.invocations==2);}
    {auto s=suite("task-f6e-regression");DatasetRegistry data;populate(data,s);auto b=run(s,false),c=run(s,true);for(auto&x:c.cases)x.metrics["planning.executability"]=0.1;memory_v2::MemoryProviderRegistry p;memory_v2::MemoryViewEngine views(p);InMemoryJudgeStore store;ScriptedJudge model;LLMJudgeWorkflow w(views,store,model);auto r=w.run(s,data,b,c,subject(s.metadata),options("regression"));assert(r.state==JudgeWorkflowState::Rejected&&r.report);assert(std::find(r.report->decision.reasons.begin(),r.report->decision.reasons.end(),"deterministic_metric_regression")!=r.report->decision.reasons.end());assert(r.report->decision.rollback_revision=="baseline-r1");}
    return 0;}
