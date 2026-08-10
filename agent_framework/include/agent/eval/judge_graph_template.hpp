#pragma once

#include <memory>

#include "agent/eval/judge_workflow.hpp"
#include "agent/graph_executor/graph_executor.hpp"

namespace agent_framework::eval {

class JudgeGraphTemplate final : public WorkflowTemplate {
public:
    JudgeGraphTemplate(std::shared_ptr<LLMJudgeWorkflow> workflow,
                       EvaluationSuite suite, std::shared_ptr<DatasetRegistry> datasets,
                       CandidateEvaluationRun baseline, CandidateEvaluationRun candidate,
                       memory_v2::MemoryScope subject, JudgeWorkflowOptions options = {});
    void build(workflow::GraphBuilder& builder, const json& config) override;
    std::string get_template_name() const override;
    std::string get_template_description() const override;
    bool validate_config(const json& config) const override;
    WorkflowResult execute(GraphExecutor& graph_executor, tf::Executor& taskflow_executor,
                           const ExecutionRequest& request) override;
private:
    std::shared_ptr<LLMJudgeWorkflow> workflow_;
    EvaluationSuite suite_;
    std::shared_ptr<DatasetRegistry> datasets_;
    CandidateEvaluationRun baseline_;
    CandidateEvaluationRun candidate_;
    memory_v2::MemoryScope subject_;
    JudgeWorkflowOptions options_;
};

} // namespace agent_framework::eval
