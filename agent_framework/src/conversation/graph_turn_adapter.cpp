#include "agent/conversation/graph_turn_adapter.hpp"
namespace agent_framework::conversation {
ModelTurnOutcome GraphTurnAdapter::from_execution(const ExecutionResult&r){
 ModelTurnOutcome o;o.task_completion_verified=false;
 if(r.outputs.contains("final_answer")&&r.outputs["final_answer"].is_string())o.candidate_answer=r.outputs["final_answer"];
 const auto stop=r.outputs.value("model_stop_reason",std::string{});
 if(stop=="cancelled"||r.status==ExecutionTerminalStatus::Cancelled)o.reason=ModelTurnStopReason::Cancelled;
 else if(stop=="deadline_exceeded"||r.status==ExecutionTerminalStatus::DeadlineExceeded)o.reason=ModelTurnStopReason::DeadlineExceeded;
 else if(stop=="guard_stopped")o.reason=ModelTurnStopReason::GuardStopped;
 else if(stop=="provider_failed"||r.status==ExecutionTerminalStatus::Failed)o.reason=ModelTurnStopReason::ProviderError;
 else if(stop=="context_exhausted")o.reason=ModelTurnStopReason::ContextExhausted;
 else if(stop=="max_iterations")o.reason=ModelTurnStopReason::MaxIterations;
 else o.reason=ModelTurnStopReason::EndTurn;
 return o;
}}
