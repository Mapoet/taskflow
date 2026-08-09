#include "agent/planning/cognition_graph_template.hpp"

#include <stdexcept>

#include "agent/agent/task_state_machine.hpp"
#include "agent/internal/agent_thread_state.hpp"

namespace agent_framework::planning {

CognitionGraphTemplate::CognitionGraphTemplate(
    std::shared_ptr<MultiStageCognitionWorkflow> workflow,
    TaskIntake intake_template, memory_v2::MemoryScope subject_template,
    CognitionPipelineOptions options)
    : workflow_(std::move(workflow)), intake_template_(std::move(intake_template)),
      subject_template_(std::move(subject_template)), options_(std::move(options)) {
    if(!workflow_) throw std::invalid_argument("cognition workflow is required");
}

void CognitionGraphTemplate::build(workflow::GraphBuilder&, const json&) {
    // Runtime execution is implemented by execute(); no dependency-free build graph is exposed.
}

std::string CognitionGraphTemplate::get_template_name() const {
    return "phase4_v2_cognition";
}

std::string CognitionGraphTemplate::get_template_description() const {
    return "Durable multi-stage RoleRuntime cognition and planning workflow";
}

bool CognitionGraphTemplate::validate_config(const json& config) const {
    return config.is_object();
}

WorkflowResult CognitionGraphTemplate::execute(
    GraphExecutor&, tf::Executor&, const ExecutionRequest& request) {
    WorkflowResult output{};
    if(!request.session) {
        output.success = false;
        output.exit_code = 1;
        output.error_message = "cognition execution session is required";
        return output;
    }
    TaskIntake intake = intake_template_;
    intake.user_goal = request.session->initial_user_prompt;
    if(request.context.task_id) intake.metadata.identity.task_id = *request.context.task_id;
    if(!request.context.tenant_id.empty())
        intake.metadata.identity.tenant_id = request.context.tenant_id;
    if(intake.metadata.identity.run_id.empty())
        intake.metadata.identity.run_id = intake.metadata.identity.task_id + ":cognition-run";
    if(intake.metadata.identity.plan_id.empty())
        intake.metadata.identity.plan_id = intake.metadata.identity.task_id + ":plan";

    auto subject = subject_template_;
    subject.tenant_id = intake.metadata.identity.tenant_id;
    subject.organization_id = intake.metadata.identity.organization_id;
    subject.principal_id = intake.metadata.identity.principal_id;
    subject.project_id = intake.metadata.identity.project_id;
    subject.task_id = intake.metadata.identity.task_id;
    subject.run_id = intake.metadata.identity.run_id;

    auto options = options_;
    const auto prior_cancelled = options.cancelled;
    options.cancelled = [control = request.control, prior_cancelled] {
        return (prior_cancelled && prior_cancelled()) ||
               (control && (control->is_cancel_requested() || control->is_deadline_exceeded()));
    };
    const auto prior_events = options.event_sink;
    options.event_sink = [sink = request.event_sink, prior_events, task = intake.metadata.identity.task_id,
                          run = intake.metadata.identity.run_id](const CognitionPipelineEvent& event) {
        if(prior_events) prior_events(event);
        if(!sink) return;
        ExecutionEvent graph_event;
        graph_event.type = event.event_type == "stage_completed"
            ? ExecutionEventType::ArtifactUpdated : ExecutionEventType::TaskStatusChanged;
        graph_event.task_id = task;
        graph_event.run_id = run;
        graph_event.payload = {{"component", "cognition"},
                               {"pipeline_id", event.pipeline_id},
                               {"checkpoint_revision", event.checkpoint_revision},
                               {"stage", cognition_stage_name(event.stage)},
                               {"event_type", event.event_type},
                               {"detail", event.payload}};
        sink(graph_event);
    };

    const auto result = workflow_->run(intake, subject, options);
    output.outputs = {{"state", cognition_pipeline_state_name(result.state)},
                      {"checkpoint", encode(result.checkpoint)},
                      {"evidence", encode(result.evidence)}};
    if(result.understanding) output.outputs["understanding"] = encode(*result.understanding);
    if(result.plan) output.outputs["plan"] = encode(*result.plan);
    if(!result.clarification_questions.empty())
        output.outputs["clarification_questions"] = result.clarification_questions;
    output.success = result.state == CognitionPipelineState::Approved ||
                     result.state == CognitionPipelineState::AwaitingApproval ||
                     result.state == CognitionPipelineState::AwaitingClarification;
    output.exit_code = output.success ? 0 : 1;
    if(!output.success)
        output.error_message = result.error_code + ":" + result.error_message;
    return output;
}

}  // namespace agent_framework::planning
