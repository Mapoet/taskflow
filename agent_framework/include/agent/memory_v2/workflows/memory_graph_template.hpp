#pragma once

#include <memory>

#include "agent/graph_executor/graph_executor.hpp"
#include "agent/memory_v2/workflows/memory_workflow.hpp"

namespace agent_framework::memory_v2::workflows {

class MemoryWorkflowGraphTemplate final : public WorkflowTemplate {
public:
    MemoryWorkflowGraphTemplate(std::shared_ptr<MultiLayerMemoryWorkflow> workflow,
                                MemoryWorkflowInput input_template,
                                MemoryWorkflowOptions options = {});

    void build(workflow::GraphBuilder& builder, const json& config) override;
    std::string get_template_name() const override;
    std::string get_template_description() const override;
    bool validate_config(const json& config) const override;
    WorkflowResult execute(GraphExecutor& graph_executor,
                           tf::Executor& executor,
                           const ExecutionRequest& request) override;

private:
    std::shared_ptr<MultiLayerMemoryWorkflow> workflow_;
    MemoryWorkflowInput input_template_;
    MemoryWorkflowOptions options_;
};

}  // namespace agent_framework::memory_v2::workflows
