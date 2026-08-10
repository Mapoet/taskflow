#include "agent/live/role_certification_graph_template.hpp"

#include <stdexcept>

namespace agent_framework::live
{
    RoleCertificationGraphTemplate::RoleCertificationGraphTemplate(std::shared_ptr<RoleLiveCertificationWorkflow> w, LiveEnvironmentProfile e, RoleLiveMatrix m, RoleCertificationOptions o) : workflow_(std::move(w)), environment_(std::move(e)), matrix_(std::move(m)), options_(std::move(o))
    {
        if (!workflow_)
            throw std::invalid_argument("live certification workflow is required");
    }
    void RoleCertificationGraphTemplate::build(workflow::GraphBuilder &, const json &) {}
    std::string RoleCertificationGraphTemplate::get_template_name() const { return "phase4_v2_role_live_certification"; }
    std::string RoleCertificationGraphTemplate::get_template_description() const { return "Durable no-skip real-role and production Live certification"; }
    bool RoleCertificationGraphTemplate::validate_config(const json &c) const { return c.is_object(); }
    WorkflowResult RoleCertificationGraphTemplate::execute(GraphExecutor &, tf::Executor &, const ExecutionRequest &r)
    {
        WorkflowResult out{};
        if (!r.session)
        {
            out.success = false;
            out.exit_code = 1;
            out.error_message = "live certification execution session is required";
            return out;
        }
        if ((r.context.task_id && *r.context.task_id != environment_.metadata.identity.task_id) || (!r.context.tenant_id.empty() && r.context.tenant_id != environment_.metadata.identity.tenant_id))
        {
            out.success = false;
            out.exit_code = 1;
            out.error_message = "live certification is bound to another tenant/task";
            return out;
        }
        auto options = options_;
        auto prior = options.cancelled;
        options.cancelled = [control = r.control, prior]
        { return (prior && prior()) || (control && (control->is_cancel_requested() || control->is_deadline_exceeded())); };
        auto result = workflow_->run(environment_, matrix_, options);
        out.outputs = {{"state", role_certification_state_name(result.state)}, {"checkpoint", encode(result.checkpoint)}};
        if (result.report)
            out.outputs["certification_report"] = encode(*result.report);
        out.success = result.state == RoleCertificationState::Certified;
        out.exit_code = out.success ? 0 : 2;
        if (!out.success)
            out.error_message = result.error_code.empty() ? "live certification did not certify" : result.error_code + ":" + result.error_message;
        return out;
    }
} // namespace agent_framework::live
