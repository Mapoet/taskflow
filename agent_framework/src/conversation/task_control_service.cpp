#include "agent/conversation/task_control_service.hpp"
namespace agent_framework::conversation {
TaskControlService::TaskControlService(TaskRegistry& t,tool_runtime::InvocationStore& i,
 tool_runtime::ExecutionControlStore& c,tool_runtime::IncrementalResultViewAssembler* r)
 :tasks_(t),invocations_(i),controls_(c),results_(r){}
nlohmann::json TaskControlService::status(const ConversationIdentity& identity,std::string_view task_id){
 auto task=tasks_.load(identity,task_id);if(!task)return{{"found",false},{"task_id",task_id}};
 nlohmann::json runs=nlohmann::json::array();
 for(const auto&link:tasks_.runs(identity,task_id)){
  auto values=invocations_.query({identity.tenant_id,{},link.run_id,{},256});nlohmann::json rows=nlohmann::json::array();
  for(const auto&v:values){auto partials=invocations_.partial_results(v.invocation_id);nlohmann::json row={{"invocation_id",v.invocation_id},{"state",tool_runtime::name(v.state)},{"revision",v.revision},{"progress_sequence",v.progress_sequence}};
   if(auto progress=invocations_.latest_progress(v.invocation_id))row["progress"]=tool_runtime::encode(*progress);
   if(results_&&!partials.empty())row["partial_output"]=results_->assemble(identity.tenant_id,partials);
   rows.push_back(std::move(row));}
  runs.push_back({{"run_id",link.run_id},{"state",link.state},{"plan_revision",link.plan_revision},{"invocations",std::move(rows)}});}
 return{{"found",true},{"task",encode(*task)},{"runs",std::move(runs)}};
}
TaskControlResult TaskControlService::cancel(const ConversationIdentity& identity,std::string_view task_id,std::string reason,std::int64_t now_ms){
 TaskControlResult result;for(const auto&link:tasks_.runs(identity,task_id))for(const auto&v:invocations_.query({identity.tenant_id,{},link.run_id,{},1024})){
  if(tool_runtime::terminal(v.state))continue;
  auto c=controls_.request_cancel(v.invocation_id,reason,now_ms);
  if(c)++result.affected;else result.errors.push_back(v.invocation_id+":"+c.error);}
 result.ok=result.errors.empty();return result;
}
}
