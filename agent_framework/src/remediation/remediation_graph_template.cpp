#include "agent/remediation/remediation_graph_template.hpp"

#include <stdexcept>

namespace agent_framework::remediation {

RemediationGraphTemplate::RemediationGraphTemplate(
    std::shared_ptr<LLMRemediationWorkflow> workflow,
    planning::ExecutionPlan current_plan, assurance::AcceptanceContract contract,
    assurance::AcceptanceReport report, assurance::AssuranceCheckpoint assurance_checkpoint,
    ImpactInventory inventory, memory_v2::MemoryScope subject, RemediationWorkflowOptions options)
    : workflow_(std::move(workflow)), current_plan_(std::move(current_plan)),
      contract_(std::move(contract)), report_(std::move(report)),
      assurance_checkpoint_(std::move(assurance_checkpoint)), inventory_(std::move(inventory)),
      subject_(std::move(subject)), options_(std::move(options)) {
    if(!workflow_) throw std::invalid_argument("remediation workflow is required");
}
void RemediationGraphTemplate::build(workflow::GraphBuilder&, const json&) {}
std::string RemediationGraphTemplate::get_template_name() const {
    return "phase4_v2_remediation";
}
std::string RemediationGraphTemplate::get_template_description() const {
    return "Durable RoleRuntime remediation, bounded replan and selective reverification";
}
bool RemediationGraphTemplate::validate_config(const json& config) const {
    return config.is_object();
}
WorkflowResult RemediationGraphTemplate::execute(
    GraphExecutor&, tf::Executor&, const ExecutionRequest& request) {
    WorkflowResult output{};
    if(!request.session) {
        output.success = false; output.exit_code = 1;
        output.error_message = "remediation execution session is required"; return output;
    }
    if((request.context.task_id && *request.context.task_id != current_plan_.metadata.identity.task_id) ||
       (!request.context.tenant_id.empty() &&
        request.context.tenant_id != current_plan_.metadata.identity.tenant_id)) {
        output.success = false; output.exit_code = 1;
        output.error_message = "remediation template inputs are digest-bound to another tenant/task";
        return output;
    }
    auto options = options_;
    const auto prior_cancelled = options.cancelled;
    options.cancelled = [control = request.control, prior_cancelled] {
        return (prior_cancelled && prior_cancelled()) ||
               (control && (control->is_cancel_requested() || control->is_deadline_exceeded()));
    };
    const auto result = workflow_->run(current_plan_, contract_, report_, assurance_checkpoint_,
                                       inventory_, subject_, options);
    output.outputs = {{"state", remediation_state_name(result.state)},
                      {"checkpoint", encode(result.checkpoint)}};
    if(result.impact_graph) output.outputs["impact_graph"] = encode(*result.impact_graph);
    if(result.remediation_plan) output.outputs["remediation_plan"] = encode(*result.remediation_plan);
    if(result.proposed_plan) output.outputs["proposed_plan"] = planning::encode(*result.proposed_plan);
    if(result.reverification_plan) output.outputs["reverification_plan"] = encode(*result.reverification_plan);
    output.success = result.state == RemediationState::ReadyForExecution ||
                     result.state == RemediationState::AwaitingApproval ||
                     result.state == RemediationState::ManualReview;
    output.exit_code = output.success ? 0 : 1;
    if(!output.success) output.error_message = result.error_code + ":" + result.error_message;
    return output;
}

}  // namespace agent_framework::remediation
