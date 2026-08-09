#pragma once

#include <memory>

#include "agent/graph_executor/graph_executor.hpp"
#include "agent/remediation/remediation_workflow.hpp"

namespace agent_framework::remediation {

class RemediationGraphTemplate final : public WorkflowTemplate {
public:
    RemediationGraphTemplate(std::shared_ptr<LLMRemediationWorkflow> workflow,
                             planning::ExecutionPlan current_plan,
                             assurance::AcceptanceContract contract,
                             assurance::AcceptanceReport report,
                             assurance::AssuranceCheckpoint assurance_checkpoint,
                             ImpactInventory inventory,
                             memory_v2::MemoryScope subject,
                             RemediationWorkflowOptions options = {});
    void build(workflow::GraphBuilder& builder, const json& config) override;
    std::string get_template_name() const override;
    std::string get_template_description() const override;
    bool validate_config(const json& config) const override;
    WorkflowResult execute(GraphExecutor& graph_executor, tf::Executor& executor,
                           const ExecutionRequest& request) override;
private:
    std::shared_ptr<LLMRemediationWorkflow> workflow_;
    planning::ExecutionPlan current_plan_;
    assurance::AcceptanceContract contract_;
    assurance::AcceptanceReport report_;
    assurance::AssuranceCheckpoint assurance_checkpoint_;
    ImpactInventory inventory_;
    memory_v2::MemoryScope subject_;
    RemediationWorkflowOptions options_;
};

}  // namespace agent_framework::remediation
