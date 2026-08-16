#include "agent/conversation/task_planning_service.hpp"

#include <algorithm>

#include "agent/contracts/contract.hpp"

namespace agent_framework::conversation {
namespace {
std::string digest(const nlohmann::json& value) {
    return contracts::canonical_digest(value).value_or("");
}
}

TaskPlanningResult TaskPlanningService::plan(
    const PersistentTask& task,
    const TaskPlanningPolicy& policy,
    const planning::CognitionPipelineOptions& requested_options) {
    TaskPlanningResult output;
    if(task.identity.tenant_id.empty() || task.identity.conversation_id.empty() ||
       task.task_id.empty() || task.current_run_id.empty() ||
       task.requirement_revision == 0) {
        output.error_code = "task_planning_contract_invalid";
        return output;
    }
    if(!policy.acceptance_contract || policy.acceptance_contract->revision == 0 ||
       policy.acceptance_contract->criteria.empty()) {
        output.error_code = policy.acceptance_contract
            ? "production_acceptance_contract_invalid"
            : "production_acceptance_contract_required";
        return output;
    }
    const auto requirements = tasks_.requirements(task.identity, task.task_id);
    if(requirements.size() != task.requirement_revision || requirements.empty()) {
        output.error_code = "task_requirement_chain_incomplete";
        return output;
    }
    for(const auto& run : tasks_.runs(task.identity, task.task_id)) {
        if(run.run_id == task.current_run_id &&
           run.requirement_revision == task.requirement_revision &&
           run.plan_revision == task.plan_revision && run.plan_revision > 0 &&
           !run.plan_digest.empty() && !run.task_contract_digest.empty()) {
            output.state = TaskPlanningState::Planned;
            output.task_revision = task.revision;
            output.plan_revision = run.plan_revision;
            output.plan_digest = run.plan_digest;
            output.task_contract_digest = run.task_contract_digest;
            contracts::ContractIdentity identity;
            identity.tenant_id = task.identity.tenant_id;
            identity.principal_id = task.identity.conversation_id;
            identity.task_id = task.task_id;
            identity.run_id = task.current_run_id;
            identity.plan_id = task.task_id + ":plan";
            if(const auto context = inputs_.task_context(identity, run.plan_digest))
            {
                output.context_projection_digest =
                    context->value("context_projection_digest", "");
                output.acceptance_contract_digest =
                    context->value("acceptance_contract_digest", "");
            }
            return output;
        }
    }
    nlohmann::json contract = {{"schema", "agent.task_contract/v1"},
        {"tenant_id", task.identity.tenant_id},
        {"conversation_id", task.identity.conversation_id},
        {"task_id", task.task_id},
        {"requirement_revision", task.requirement_revision},
        {"requirements", nlohmann::json::array()}};
    for(const auto& requirement : requirements)
        contract["requirements"].push_back({
            {"revision", requirement.revision}, {"intent", name(requirement.intent)},
            {"turn_id", requirement.turn_id}, {"content", requirement.content},
            {"digest", requirement.digest}});
    output.task_contract_digest = digest(contract);
    if(output.task_contract_digest.empty()) {
        output.error_code = "task_contract_digest_failed";
        return output;
    }

    planning::TaskIntake intake;
    intake.metadata.identity.tenant_id = task.identity.tenant_id;
    intake.metadata.identity.principal_id = task.identity.conversation_id;
    intake.metadata.identity.task_id = task.task_id;
    intake.metadata.identity.run_id = task.current_run_id;
    intake.metadata.identity.plan_id = task.task_id + ":plan";
    intake.user_goal = requirements.front().content;
    for(std::size_t index = 1; index < requirements.size(); ++index)
        intake.explicit_constraints.push_back(requirements[index].content);
    intake.requested_deliverables = policy.requested_deliverables;
    intake.granted_authorities = policy.granted_authorities;
    intake.success_signals = policy.success_signals;
    intake.fact_gaps = policy.fact_gaps;
    const auto intake_commit = inputs_.put_intake(intake, task.requirement_revision);
    if(!intake_commit) {
        output.error_code = "production_intake_commit_failed";
        output.error_message = intake_commit.error;
        return output;
    }

    memory_v2::MemoryScope subject;
    subject.tenant_id = intake.metadata.identity.tenant_id;
    subject.principal_id = intake.metadata.identity.principal_id;
    subject.task_id = intake.metadata.identity.task_id;
    subject.run_id = intake.metadata.identity.run_id;
    auto options = requested_options;
    if(options.pipeline_id.empty())
        options.pipeline_id = task.task_id + ":requirements:" +
                              std::to_string(task.requirement_revision);
    if(task.plan_revision > 0 && options.plan_revision_request.empty())
        options.plan_revision_request = {
            {"schema", "agent.requirement_plan_supersession/v1"},
            {"reason", "requirement_revision_advanced"},
            {"prior_plan_revision", task.plan_revision},
            {"requirement_revision", task.requirement_revision},
            {"task_contract_digest", output.task_contract_digest}};
    const auto planned = cognition_.run(intake, subject, options);
    if(planned.state == planning::CognitionPipelineState::AwaitingClarification) {
        output.state = TaskPlanningState::AwaitingClarification;
        output.clarification_questions = planned.clarification_questions;
        return output;
    }
    if(planned.state == planning::CognitionPipelineState::AwaitingApproval) {
        output.state = TaskPlanningState::AwaitingApproval;
        return output;
    }
    if(planned.state != planning::CognitionPipelineState::Approved || !planned.plan) {
        output.error_code = planned.error_code.empty()
            ? "cognition_plan_not_approved" : planned.error_code;
        output.error_message = planned.error_message;
        return output;
    }
    const auto plan_document = planning::encode(*planned.plan);
    output.plan_digest = plan_document.value("canonical_digest", "");
    output.plan_revision = planned.plan->plan_revision;
    if(output.plan_digest.empty() || output.plan_revision == 0) {
        output.error_code = "approved_plan_digest_invalid";
        return output;
    }
    if(policy.executor_id.empty() || policy.executor_revision.empty()) {
        output.error_code = "production_plan_executor_policy_missing";
        return output;
    }
    for(const auto& node : planned.plan->nodes) {
        tool_runtime::PlanNodeExecutionDescriptor descriptor;
        descriptor.metadata = planned.plan->metadata;
        descriptor.plan_digest = output.plan_digest;
        descriptor.node_id = node.node_id;
        descriptor.executor_id = policy.executor_id;
        descriptor.executor_revision = policy.executor_revision;
        descriptor.input = {{"objective", node.objective},
            {"input_contracts", node.input_contracts},
            {"output_contracts", node.output_contracts},
            {"acceptance_contract_id", node.acceptance_contract_id}};
        for(const auto& capability : node.required_capabilities) {
            if(std::find(policy.granted_authorities.begin(),
                         policy.granted_authorities.end(), capability) ==
               policy.granted_authorities.end()) {
                output.error_code = "plan_node_capability_not_granted";
                output.error_message = node.node_id + ":" + capability;
                return output;
            }
            descriptor.granted_capabilities.push_back(capability);
        }
        descriptor.side_effecting = !node.side_effects.empty();
        const nlohmann::json descriptor_document = {
            {"plan_digest", descriptor.plan_digest}, {"node_id", descriptor.node_id},
            {"executor_id", descriptor.executor_id},
            {"executor_revision", descriptor.executor_revision},
            {"input", descriptor.input},
            {"granted_capabilities", descriptor.granted_capabilities},
            {"approval_decision_id", descriptor.approval_decision_id},
            {"side_effecting", descriptor.side_effecting}};
        descriptor.descriptor_digest = digest(descriptor_document);
        const auto descriptor_commit = inputs_.put_plan_node_descriptor(
            descriptor, output.plan_revision);
        if(!descriptor_commit) {
            output.error_code = "plan_node_descriptor_commit_failed";
            output.error_message = node.node_id + ":" + descriptor_commit.error;
            return output;
        }
    }
    const auto evidence_document = planning::encode(planned.evidence);
    const auto understanding_document = planned.understanding
        ? planning::encode(*planned.understanding) : nlohmann::json::object();
    ContextProjectionManifest projection;
    projection.identity = task.identity;
    projection.turn_id = task.current_turn_id;
    projection.revision = output.plan_revision;
    projection.profile_revision_digest = policy.executor_revision;
    projection.prompt_revision_digest = output.plan_digest;
    projection.segments = {
        {"task_contract", "production-input://task-contract/" + task.task_id,
         output.task_contract_digest, "persistent_task_registry", "", 0, true},
        {"current_plan", "production-input://plan/" + output.plan_digest,
         output.plan_digest, "cognition_plan_store", "", 0, true},
        {"evidence_bundle", "production-input://evidence/" + task.task_id,
         digest(evidence_document), "cognition_evidence_store", "", 0, true},
        {"task_understanding", "production-input://understanding/" + task.task_id,
         digest(understanding_document), "cognition_workflow", "", 0, true}};
    std::string projection_error;
    const auto projected = ContextProjector::build(
        std::move(projection), &projection_error);
    if(!projected) {
        output.error_code = "context_projection_build_failed";
        output.error_message = projection_error;
        return output;
    }
    output.context_projection_digest = projected->digest;
    auto acceptance_contract = *policy.acceptance_contract;
    acceptance_contract.metadata = planned.plan->metadata;
    acceptance_contract.plan_digest = output.plan_digest;
    const auto acceptance_document = assurance::encode(acceptance_contract);
    const auto acceptance_commit = inputs_.put_acceptance_contract(acceptance_contract);
    if(!acceptance_commit) {
        output.error_code = "production_acceptance_contract_commit_failed";
        output.error_message = acceptance_commit.error;
        return output;
    }
    output.acceptance_contract_digest =
        acceptance_document.value("canonical_digest", "");
    if(output.acceptance_contract_digest.empty()) {
        output.error_code = "production_acceptance_contract_digest_missing";
        return output;
    }
    const auto context_commit = inputs_.put_task_context(
        intake.metadata.identity, output.plan_digest,
        {{"schema", "agent.production_task_context/v1"},
         {"task_contract_digest", output.task_contract_digest},
         {"task_contract", contract}, {"plan", plan_document},
         {"evidence_bundle", evidence_document},
         {"understanding", understanding_document},
         {"context_projection", encode(*projected)},
         {"context_projection_digest", projected->digest},
         {"acceptance_contract_digest", output.acceptance_contract_digest}},
        output.plan_revision);
    if(!context_commit) {
        output.error_code = "production_task_context_commit_failed";
        output.error_message = context_commit.error;
        return output;
    }
    const auto bound = orchestrator_.bind_plan(
        {task.identity, task.task_id, task.current_run_id,
         task.requirement_revision, output.plan_revision, output.plan_digest,
         output.task_contract_digest}, task.revision);
    if(!bound.ok) {
        output.error_code = "task_plan_bind_failed";
        output.error_message = bound.error;
        return output;
    }
    output.task_revision = bound.revision;
    output.state = TaskPlanningState::Planned;
    return output;
}

}  // namespace agent_framework::conversation
