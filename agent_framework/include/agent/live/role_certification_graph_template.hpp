#pragma once

#include <memory>

#include "agent/graph_executor/graph_executor.hpp"
#include "agent/live/role_certification.hpp"

namespace agent_framework::live {

class RoleCertificationGraphTemplate final : public WorkflowTemplate {
public:
    RoleCertificationGraphTemplate(std::shared_ptr<RoleLiveCertificationWorkflow> workflow,
                                   LiveEnvironmentProfile environment,
                                   RoleLiveMatrix matrix,
                                   RoleCertificationOptions options);
    void build(workflow::GraphBuilder& builder, const json& config) override;
    std::string get_template_name() const override;
    std::string get_template_description() const override;
    bool validate_config(const json& config) const override;
    WorkflowResult execute(GraphExecutor& graph_executor, tf::Executor& taskflow_executor,
                           const ExecutionRequest& request) override;
private:
    std::shared_ptr<RoleLiveCertificationWorkflow> workflow_;
    LiveEnvironmentProfile environment_;
    RoleLiveMatrix matrix_;
    RoleCertificationOptions options_;
};

} // namespace agent_framework::live
