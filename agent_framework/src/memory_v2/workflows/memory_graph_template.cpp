#include "agent/memory_v2/workflows/memory_graph_template.hpp"

#include <stdexcept>

#include "agent/internal/agent_thread_state.hpp"

namespace agent_framework::memory_v2::workflows {

MemoryWorkflowGraphTemplate::MemoryWorkflowGraphTemplate(
    std::shared_ptr<MultiLayerMemoryWorkflow> workflow,
    MemoryWorkflowInput input_template, MemoryWorkflowOptions options)
    : workflow_(std::move(workflow)), input_template_(std::move(input_template)),
      options_(std::move(options)) {
    if(!workflow_) throw std::invalid_argument("memory workflow is required");
}

void MemoryWorkflowGraphTemplate::build(workflow::GraphBuilder&, const json&) {
    // Execution is supplied by execute(); semantic stages remain behind RoleRuntime.
}

std::string MemoryWorkflowGraphTemplate::get_template_name() const {
    return "phase4_v2_memory";
}

std::string MemoryWorkflowGraphTemplate::get_template_description() const {
    return "Durable LLM-driven multi-layer memory candidate and governance workflow";
}

bool MemoryWorkflowGraphTemplate::validate_config(const json& config) const {
    return config.is_object();
}

WorkflowResult MemoryWorkflowGraphTemplate::execute(
    GraphExecutor&, tf::Executor&, const ExecutionRequest& request) {
    WorkflowResult output{};
    if(!request.session) {
        output.success = false;
        output.exit_code = 1;
        output.error_message = "memory workflow execution session is required";
        return output;
    }
    auto input = input_template_;
    if(request.context.task_id) input.metadata.identity.task_id = *request.context.task_id;
    if(!request.context.tenant_id.empty()) input.metadata.identity.tenant_id = request.context.tenant_id;
    if(input.metadata.identity.run_id.empty())
        input.metadata.identity.run_id = input.metadata.identity.task_id + ":memory-run";
    if(input.workflow_id.empty()) input.workflow_id = input.metadata.identity.run_id + ":memory";
    input.subject.tenant_id = input.metadata.identity.tenant_id;
    input.subject.organization_id = input.metadata.identity.organization_id;
    input.subject.principal_id = input.metadata.identity.principal_id;
    input.subject.project_id = input.metadata.identity.project_id;
    input.subject.task_id = input.metadata.identity.task_id;
    input.subject.run_id = input.metadata.identity.run_id;

    MemorySourceArtifact source;
    source.artifact_id = input.workflow_id + ":user-turn";
    source.source_kind = "user_instruction";
    source.source_locator = "session.initial_user_prompt";
    source.source_digest = contracts::canonical_digest(
        json{{"text", request.session->initial_user_prompt}}).value_or("");
    source.scope = input.subject;
    source.scope.level = MemoryLevel::Turn;
    source.content = {{"text", request.session->initial_user_prompt}};
    source.trusted_instruction = true;
    input.sources.push_back(std::move(source));

    auto options = options_;
    const auto previous_cancelled = options.cancelled;
    options.cancelled = [control = request.control, previous_cancelled] {
        return (previous_cancelled && previous_cancelled()) ||
               (control && (control->is_cancel_requested() || control->is_deadline_exceeded()));
    };
    const auto previous_events = options.event_sink;
    options.event_sink = [sink = request.event_sink, previous_events,
                          task = input.metadata.identity.task_id,
                          run = input.metadata.identity.run_id](const MemoryWorkflowEvent& event) {
        if(previous_events) previous_events(event);
        if(!sink) return;
        ExecutionEvent graph_event;
        graph_event.type = event.event_type == "stage_completed"
            ? ExecutionEventType::ArtifactUpdated : ExecutionEventType::TaskStatusChanged;
        graph_event.task_id = task;
        graph_event.run_id = run;
        graph_event.payload = {{"component", "memory_workflow"},
                               {"workflow_id", event.workflow_id},
                               {"checkpoint_revision", event.checkpoint_revision},
                               {"stage", memory_workflow_stage_name(event.stage)},
                               {"event_type", event.event_type},
                               {"detail", event.payload}};
        sink(graph_event);
    };

    const auto result = workflow_->run(input, options);
    output.outputs = {{"state", memory_workflow_state_name(result.state)},
                      {"checkpoint", encode(result.checkpoint)},
                      {"recommendations", result.recommendations}};
    output.success = result.state == MemoryWorkflowState::Completed ||
                     result.state == MemoryWorkflowState::AwaitingApproval ||
                     result.state == MemoryWorkflowState::AwaitingClarification;
    output.exit_code = output.success ? 0 : 1;
    if(!output.success) output.error_message = result.error_code + ":" + result.error_message;
    return output;
}

}  // namespace agent_framework::memory_v2::workflows
