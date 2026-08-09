#pragma once

#include <memory>

#include "agent/graph_executor/graph_executor.hpp"
#include "agent/planning/cognition_pipeline.hpp"

namespace agent_framework::planning {

// Executable GraphExecutor adapter.  The template owns no provider client; all semantic
// stages continue through the pipeline's CognitionStageModel/RoleRuntime boundary.
class CognitionGraphTemplate final : public WorkflowTemplate {
public:
    CognitionGraphTemplate(std::shared_ptr<MultiStageCognitionWorkflow> workflow,
                           TaskIntake intake_template,
                           memory_v2::MemoryScope subject_template,
                           CognitionPipelineOptions options = {});

    void build(workflow::GraphBuilder& builder, const json& config) override;
    std::string get_template_name() const override;
    std::string get_template_description() const override;
    bool validate_config(const json& config) const override;
    WorkflowResult execute(GraphExecutor& graph_executor,
                           tf::Executor& executor,
                           const ExecutionRequest& request) override;

private:
    std::shared_ptr<MultiStageCognitionWorkflow> workflow_;
    TaskIntake intake_template_;
    memory_v2::MemoryScope subject_template_;
    CognitionPipelineOptions options_;
};

}  // namespace agent_framework::planning
