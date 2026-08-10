#include "agent/eval/judge_graph_template.hpp"

#include <stdexcept>

namespace agent_framework::eval
{

    JudgeGraphTemplate::JudgeGraphTemplate(std::shared_ptr<LLMJudgeWorkflow> workflow,
                                           EvaluationSuite suite, std::shared_ptr<DatasetRegistry> datasets,
                                           CandidateEvaluationRun baseline, CandidateEvaluationRun candidate,
                                           memory_v2::MemoryScope subject, JudgeWorkflowOptions options)
        : workflow_(std::move(workflow)), suite_(std::move(suite)), datasets_(std::move(datasets)),
          baseline_(std::move(baseline)), candidate_(std::move(candidate)), subject_(std::move(subject)),
          options_(std::move(options))
    {
        if (!workflow_ || !datasets_)
            throw std::invalid_argument("judge workflow and dataset registry are required");
    }
    void JudgeGraphTemplate::build(workflow::GraphBuilder &, const json &) {}
    std::string JudgeGraphTemplate::get_template_name() const { return "phase4_v2_judge_evaluation"; }
    std::string JudgeGraphTemplate::get_template_description() const { return "Durable blind multi-Judge evaluation, calibration and governed upgrade gate"; }
    bool JudgeGraphTemplate::validate_config(const json &config) const { return config.is_object(); }
    WorkflowResult JudgeGraphTemplate::execute(GraphExecutor &, tf::Executor &, const ExecutionRequest &request)
    {
        WorkflowResult out{};
        if (!request.session)
        {
            out.success = false;
            out.exit_code = 1;
            out.error_message = "judge execution session is required";
            return out;
        }
        if ((request.context.task_id && *request.context.task_id != suite_.metadata.identity.task_id) ||
            (!request.context.tenant_id.empty() && request.context.tenant_id != suite_.metadata.identity.tenant_id))
        {
            out.success = false;
            out.exit_code = 1;
            out.error_message = "judge template inputs are digest-bound to another tenant/task";
            return out;
        }
        auto options = options_;
        auto prior = options.cancelled;
        options.cancelled = [control = request.control, prior]
        { return (prior && prior()) || (control && (control->is_cancel_requested() || control->is_deadline_exceeded())); };
        auto result = workflow_->run(suite_, *datasets_, baseline_, candidate_, subject_, options);
        out.outputs = {{"state", judge_workflow_state_name(result.state)}, {"checkpoint", encode(result.checkpoint)}};
        if (result.report)
            out.outputs["evaluation_report"] = encode(*result.report);
        out.success = result.state == JudgeWorkflowState::Approved || result.state == JudgeWorkflowState::Rejected ||
                      result.state == JudgeWorkflowState::ManualReview || result.state == JudgeWorkflowState::AwaitingApproval;
        out.exit_code = out.success ? 0 : 1;
        if (!out.success)
            out.error_message = result.error_code + ":" + result.error_message;
        return out;
    }

} // namespace agent_framework::eval
