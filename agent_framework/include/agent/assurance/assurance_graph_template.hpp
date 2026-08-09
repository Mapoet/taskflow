#pragma once

#include <memory>

#include "agent/assurance/professional_workflow.hpp"
#include "agent/graph_executor/graph_executor.hpp"

namespace agent_framework::assurance {

class AssuranceGraphTemplate final : public WorkflowTemplate {
public:
    AssuranceGraphTemplate(std::shared_ptr<ProfessionalAssuranceWorkflow> workflow,
                           AcceptanceContract contract_template,
                           memory_v2::MemoryScope subject_template,
                           nlohmann::json task_context_template = nlohmann::json::object(),
                           nlohmann::json artifact_manifest_template = nlohmann::json::object(),
                           AssuranceWorkflowOptions options = {});

    void build(workflow::GraphBuilder& builder, const json& config) override;
    std::string get_template_name() const override;
    std::string get_template_description() const override;
    bool validate_config(const json& config) const override;
    WorkflowResult execute(GraphExecutor& graph_executor, tf::Executor& executor,
                           const ExecutionRequest& request) override;

private:
    std::shared_ptr<ProfessionalAssuranceWorkflow> workflow_;
    AcceptanceContract contract_template_;
    memory_v2::MemoryScope subject_template_;
    nlohmann::json task_context_template_;
    nlohmann::json artifact_manifest_template_;
    AssuranceWorkflowOptions options_;
};

}  // namespace agent_framework::assurance
