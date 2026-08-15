#include "agent/conversation/graph_turn_adapter.hpp"

namespace agent_framework::conversation {
namespace {

void apply_receipts(ModelTurnOutcome& o, const nlohmann::json& outputs)
{
    if (!outputs.contains("tool_receipt_refs") || !outputs["tool_receipt_refs"].is_array())
        return;
    for (const auto& item : outputs["tool_receipt_refs"]) {
        if (item.is_string() && !item.get_ref<const std::string&>().empty())
            o.tool_receipt_refs.push_back(item.get<std::string>());
    }
}

void apply_answer(ModelTurnOutcome& o, const nlohmann::json& outputs)
{
    if (outputs.contains("final_answer") && outputs["final_answer"].is_string())
        o.candidate_answer = outputs["final_answer"].get<std::string>();
}

ModelTurnStopReason reason_from_stop(std::string_view stop,
                                     bool failed,
                                     agent_framework::ExecutionTerminalStatus status =
                                         agent_framework::ExecutionTerminalStatus::Completed)
{
    if (stop == "cancelled" ||
        status == agent_framework::ExecutionTerminalStatus::Cancelled)
        return ModelTurnStopReason::Cancelled;
    if (stop == "deadline_exceeded" ||
        status == agent_framework::ExecutionTerminalStatus::DeadlineExceeded)
        return ModelTurnStopReason::DeadlineExceeded;
    if (stop == "guard_stopped")
        return ModelTurnStopReason::GuardStopped;
    if (stop == "empty_delivery")
        return ModelTurnStopReason::ProviderError;
    if (stop == "provider_failed" ||
        status == agent_framework::ExecutionTerminalStatus::Failed || failed)
        return ModelTurnStopReason::ProviderError;
    if (stop == "context_exhausted")
        return ModelTurnStopReason::ContextExhausted;
    if (stop == "max_iterations")
        return ModelTurnStopReason::MaxIterations;
    return ModelTurnStopReason::EndTurn;
}

} // namespace

ModelTurnOutcome GraphTurnAdapter::from_execution(const ExecutionResult& r)
{
    ModelTurnOutcome o;
    o.task_completion_verified = false;
    apply_answer(o, r.outputs);
    apply_receipts(o, r.outputs);
    const auto stop = r.outputs.value("model_stop_reason", std::string{});
    o.reason = reason_from_stop(stop, !r.success || r.error.has_value(), r.status);
    return o;
}

ModelTurnOutcome GraphTurnAdapter::from_workflow(const WorkflowResult& r)
{
    ModelTurnOutcome o;
    o.task_completion_verified = false;
    apply_answer(o, r.outputs);
    apply_receipts(o, r.outputs);
    const auto stop = r.outputs.value("model_stop_reason", std::string{});
    const bool failed = !r.success || r.error_message.has_value();
    if (stop == "cancelled" || r.exit_code == 130)
        o.reason = ModelTurnStopReason::Cancelled;
    else if (stop == "deadline_exceeded")
        o.reason = ModelTurnStopReason::DeadlineExceeded;
    else if (stop == "guard_stopped" || r.exit_code == 4)
        o.reason = ModelTurnStopReason::GuardStopped;
    else if (stop == "empty_delivery")
        o.reason = ModelTurnStopReason::ProviderError;
    else if (stop == "context_exhausted")
        o.reason = ModelTurnStopReason::ContextExhausted;
    else if (stop == "max_iterations")
        o.reason = ModelTurnStopReason::MaxIterations;
    else if (failed)
        o.reason = ModelTurnStopReason::ProviderError;
    else
        o.reason = ModelTurnStopReason::EndTurn;
    // Operational diagnostics stay on WorkflowResult.error_message.  Copying
    // them into candidate_answer leaks internal codes into the transcript.
    return o;
}

} // namespace agent_framework::conversation
