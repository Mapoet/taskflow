#include "agent/assurance/assurance_graph_template.hpp"

#include <stdexcept>

namespace agent_framework::assurance {

AssuranceGraphTemplate::AssuranceGraphTemplate(
    std::shared_ptr<ProfessionalAssuranceWorkflow> workflow,
    AcceptanceContract contract_template, memory_v2::MemoryScope subject_template,
    nlohmann::json task_context_template, nlohmann::json artifact_manifest_template,
    AssuranceWorkflowOptions options)
    : workflow_(std::move(workflow)), contract_template_(std::move(contract_template)),
      subject_template_(std::move(subject_template)),
      task_context_template_(std::move(task_context_template)),
      artifact_manifest_template_(std::move(artifact_manifest_template)),
      options_(std::move(options)) {
    if(!workflow_) throw std::invalid_argument("assurance workflow is required");
}

void AssuranceGraphTemplate::build(workflow::GraphBuilder&, const json&) {
    // Execution is delegated to the durable workflow in execute().
}

std::string AssuranceGraphTemplate::get_template_name() const {
    return "phase4_v2_professional_assurance";
}

std::string AssuranceGraphTemplate::get_template_description() const {
    return "Durable multi-role RoleRuntime professional verification and deterministic arbitration";
}

bool AssuranceGraphTemplate::validate_config(const json& config) const {
    return config.is_object();
}

WorkflowResult AssuranceGraphTemplate::execute(
    GraphExecutor&, tf::Executor&, const ExecutionRequest& request) {
    WorkflowResult output{};
    if(!request.session) {
        output.success = false;
        output.exit_code = 1;
        output.error_message = "assurance execution session is required";
        return output;
    }
    auto contract = contract_template_;
    if(request.context.task_id) contract.metadata.identity.task_id = *request.context.task_id;
    if(!request.context.tenant_id.empty())
        contract.metadata.identity.tenant_id = request.context.tenant_id;
    if(contract.metadata.identity.run_id.empty())
        contract.metadata.identity.run_id = contract.metadata.identity.task_id + ":assurance-run";
    auto subject = subject_template_;
    subject.tenant_id = contract.metadata.identity.tenant_id;
    subject.organization_id = contract.metadata.identity.organization_id;
    subject.principal_id = contract.metadata.identity.principal_id;
    subject.project_id = contract.metadata.identity.project_id;
    subject.task_id = contract.metadata.identity.task_id;
    subject.run_id = contract.metadata.identity.run_id;
    auto task_context = task_context_template_;
    task_context["user_request"] = request.session->initial_user_prompt;
    auto options = options_;
    const auto prior_cancelled = options.cancelled;
    options.cancelled = [control = request.control, prior_cancelled] {
        return (prior_cancelled && prior_cancelled()) ||
               (control && (control->is_cancel_requested() || control->is_deadline_exceeded()));
    };
    const auto prior_events = options.event_sink;
    options.event_sink = [sink = request.event_sink, prior_events,
                          task = contract.metadata.identity.task_id,
                          run = contract.metadata.identity.run_id](const AssuranceWorkflowEvent& event) {
        if(prior_events) prior_events(event);
        if(!sink) return;
        ExecutionEvent graph_event;
        graph_event.type = event.event_type == "stage_completed"
            ? ExecutionEventType::ArtifactUpdated : ExecutionEventType::TaskStatusChanged;
        graph_event.task_id = task;
        graph_event.run_id = run;
        graph_event.payload = {{"component", "professional_assurance"},
                               {"workflow_id", event.workflow_id},
                               {"checkpoint_revision", event.checkpoint_revision},
                               {"stage", assurance_stage_name(event.stage)},
                               {"event_type", event.event_type},
                               {"detail", event.payload}};
        sink(graph_event);
    };
    const auto result = workflow_->run(contract, subject, task_context,
                                       artifact_manifest_template_, options);
    output.outputs = {{"state", assurance_workflow_state_name(result.state)},
                      {"checkpoint", encode(result.checkpoint)}};
    if(result.verification_plan) output.outputs["verification_plan"] = encode(*result.verification_plan);
    if(result.resolution) output.outputs["resolution"] = encode(*result.resolution);
    if(result.report) output.outputs["acceptance_report"] = encode(*result.report);
    output.success = result.state == AssuranceWorkflowState::Completed ||
                     result.state == AssuranceWorkflowState::ManualReview;
    output.exit_code = output.success ? 0 : 1;
    if(!output.success) output.error_message = result.error_code + ":" + result.error_message;
    return output;
}

}  // namespace agent_framework::assurance
