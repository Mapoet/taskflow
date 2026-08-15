#include "agent/agent_template/runner.hpp"
#include "agent/contracts/contract.hpp"
#include "agent/skills/skill_policy.hpp"
#include <stdexcept>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
namespace agent_framework::agent_template
{
    namespace
    {
        RunnerEvent event(const RunnerRequest &r, std::uint64_t seq, RunnerLifecycleState state, nlohmann::json details = {}) { return {r.invocation.invocation_id + ":" + r.node.node_id + ":" + std::to_string(seq), seq, r.node.node_id, state, "", std::move(details)}; }
        std::string utc_now()
        {
            const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::tm tm{};
#if defined(_WIN32)
            gmtime_s(&tm, &now);
#else
            gmtime_r(&now, &tm);
#endif
            std::ostringstream out;
            out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
            return out.str();
        }
    }
    CallbackSkillRunner::CallbackSkillRunner(SkillRunnerKind k, RunnerCallback c) : kind_(k), callback_(std::move(c))
    {
        if (!callback_)
            throw std::invalid_argument("runner callback required");
    }
    RunnerResult CallbackSkillRunner::run(const RunnerRequest &r)
    {
        RunnerResult out;
        out.receipt.invocation_id = r.invocation.invocation_id;
        out.receipt.node_id = r.node.node_id;
        out.receipt.runner = kind_;
        auto cancel = r.cancel ? r.cancel : std::make_shared<std::atomic_bool>(false);
        {
            std::lock_guard l(mutex_);
            active_[r.invocation.invocation_id + ":" + r.node.node_id] = cancel;
        }
        out.events = {event(r, 1, RunnerLifecycleState::Admitted), event(r, 2, RunnerLifecycleState::Prepared), event(r, 3, RunnerLifecycleState::Running)};
        try
        {
            if (cancel->load())
                throw std::runtime_error("cancelled");
            out.output = callback_(r);
            if (cancel->load())
                throw std::runtime_error("cancelled");
            out.ok = true;
            out.receipt.terminal_state = RunnerLifecycleState::Succeeded;
            out.receipt.output_digest = contracts::embedded_digest(out.output).value_or("");
            out.events.push_back(event(r, 4, RunnerLifecycleState::Succeeded, {{"output_digest", out.receipt.output_digest}}));
        }
        catch (const std::exception &e)
        {
            out.error_message = e.what();
            out.error_code = cancel->load() ? "cancelled" : "runner_failed";
            out.receipt.terminal_state = cancel->load() ? RunnerLifecycleState::Cancelled : RunnerLifecycleState::Failed;
            out.events.push_back(event(r, 4, out.receipt.terminal_state, {{"error", out.error_message}}));
        }
        {
            std::lock_guard l(mutex_);
            active_.erase(r.invocation.invocation_id + ":" + r.node.node_id);
        }
        return out;
    }
    bool CallbackSkillRunner::cancel(std::string_view id)
    {
        std::lock_guard l(mutex_);
        auto i = active_.find(std::string(id));
        if (i == active_.end())
            return false;
        i->second->store(true);
        return true;
    }
    ToolBusSkillRunner::ToolBusSkillRunner(SkillRunnerKind kind, std::shared_ptr<ToolBus> bus) : kind_(kind), bus_(std::move(bus))
    {
        if (!bus_)
            throw std::invalid_argument("ToolBus required");
    }
    RunnerResult ToolBusSkillRunner::run(const RunnerRequest &r)
    {
        RunnerResult out;
        out.receipt.invocation_id = r.invocation.invocation_id;
        out.receipt.node_id = r.node.node_id;
        out.receipt.runner = kind_;
        if (!r.input.contains("tool") || !r.input.contains("arguments"))
        {
            out.error_code = "typed_tool_request_required";
            out.error_message = "tool and arguments are required";
            return out;
        }
        const auto requested_tool = r.input.at("tool").get<std::string>();
        const auto tool = bus_->resolve_tool_name(requested_tool);
        const auto portable_tool = portable_tool_name(tool);
        const auto allowed = std::any_of(r.session.effective_permissions.tools.begin(), r.session.effective_permissions.tools.end(),
                                         [&](const auto &grant)
                                         { return grant == "*" || portable_tool_name(bus_->resolve_tool_name(grant)) == portable_tool; });
        if (!allowed)
        {
            out.error_code = "node_tool_permission_denied";
            return out;
        }
        auto cancel = r.cancel ? r.cancel : std::make_shared<std::atomic_bool>(false);
        const auto key = r.invocation.invocation_id + ":" + r.node.node_id;
        {
            std::lock_guard lock(mutex_);
            active_[key] = cancel;
        }
        const auto input_digest = contracts::embedded_digest(r.input.at("arguments")).value_or("");
        const auto started = std::chrono::steady_clock::now();
        out.events = {event(r, 1, RunnerLifecycleState::Admitted, {{"requested_tool", requested_tool}, {"canonical_tool", tool}, {"permission_decision", "allowed"}}),
                      event(r, 2, RunnerLifecycleState::Prepared, {{"input_digest", input_digest}}),
                      event(r, 3, RunnerLifecycleState::Running)};
        ToolCallControl control;
        control.cancellation_requested = [cancel]
        { return cancel->load(); };
        try
        {
            out.output = bus_->call_tool(tool, r.input.at("arguments"), control).get();
        }
        catch (const std::exception &e)
        {
            out.output = {{"error", e.what()}, {"code", "tool_exception"}};
        }
        {
            std::lock_guard lock(mutex_);
            active_.erase(key);
        }
        const bool process_failed = out.output.value("timed_out", false) ||
                                    (out.output.contains("exit_code") && out.output.at("exit_code").is_number_integer() &&
                                     out.output.at("exit_code").get<int>() != 0);
        out.ok = !out.output.contains("error") && !process_failed && !cancel->load();
        if (cancel->load())
            out.error_code = "cancelled";
        else if (out.output.contains("error") && out.output.at("error").is_object())
            out.error_code = out.output.at("error").value("code", "tool_failed");
        else if (out.output.value("timed_out", false))
            out.error_code = "tool_timeout";
        else if (process_failed)
            out.error_code = "tool_exit_nonzero";
        else
            out.error_code.clear();
        if (!out.ok && out.error_message.empty())
        {
            if (out.output.contains("error") && out.output.at("error").is_object())
                out.error_message = out.output.at("error").value("message", "tool returned an error");
            else if (process_failed)
                out.error_message = "tool process did not exit successfully";
        }
        out.receipt.terminal_state = out.ok ? RunnerLifecycleState::Succeeded : cancel->load() ? RunnerLifecycleState::Cancelled
                                                                                               : RunnerLifecycleState::Failed;
        out.receipt.output_digest = contracts::embedded_digest(out.output).value_or("");
        const auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        out.events.push_back(event(r, 4, out.receipt.terminal_state,
                                   out.ok ? nlohmann::json{{"output_digest", out.receipt.output_digest}, {"latency_ms", latency}}
                                          : nlohmann::json{{"error_code", out.error_code}, {"tool", tool}, {"latency_ms", latency}}));
        return out;
    }
    bool ToolBusSkillRunner::cancel(std::string_view id)
    {
        std::lock_guard lock(mutex_);
        auto it = active_.find(std::string(id));
        if (it == active_.end())
            return false;
        it->second->store(true);
        return true;
    }
    ChildTaskSkillRunner::ChildTaskSkillRunner(std::shared_ptr<ChildTaskBackend> backend) : backend_(std::move(backend))
    {
        if (!backend_)
            throw std::invalid_argument("ChildTaskBackend required");
    }
    RunnerResult ChildTaskSkillRunner::run(const RunnerRequest &r)
    {
        RunnerResult out;
        out.receipt.invocation_id = r.invocation.invocation_id;
        out.receipt.node_id = r.node.node_id;
        out.receipt.runner = kind();
        ChildTaskRequest request;
        request.child_id = r.node.node_id;
        request.parent_run_id = r.invocation.invocation_id;
        request.run_id = r.invocation.invocation_id + ":" + r.node.node_id;
        request.idempotency_key = r.node.idempotency_key;
        request.inputs = r.input;
        request.policy.cancel_requested = r.cancel;
        request.grants.tools = r.session.effective_permissions.tools;
        request.grants.network = r.session.effective_permissions.network;
        request.grants.environment = r.session.effective_permissions.environment;
        request.grants.filesystem_read = r.session.effective_permissions.filesystem_read;
        request.grants.filesystem_write = r.session.effective_permissions.filesystem_write;
        request.grants.secrets = r.session.effective_permissions.secrets;
        out.events = {event(r, 1, RunnerLifecycleState::Admitted), event(r, 2, RunnerLifecycleState::Prepared), event(r, 3, RunnerLifecycleState::Running)};
        try
        {
            auto handle = backend_->start(std::move(request));
            if (!handle)
                throw std::runtime_error("child backend returned no handle");
            const auto key = r.invocation.invocation_id + ":" + r.node.node_id;
            {
                std::lock_guard lock(mutex_);
                active_[key] = handle.get();
            }
            auto result = handle->wait();
            {
                std::lock_guard lock(mutex_);
                active_.erase(key);
            }
            out.output = child_task_result_to_json(result);
            out.ok = result.ok();
            out.error_code = result.error_code;
            out.error_message = result.error.value_or("");
        }
        catch (const std::exception &e)
        {
            out.error_code = "child_task_failed";
            out.error_message = e.what();
        }
        out.receipt.terminal_state = out.ok ? RunnerLifecycleState::Succeeded : RunnerLifecycleState::Failed;
        out.receipt.output_digest = contracts::embedded_digest(out.output).value_or("");
        out.events.push_back(event(r, 4, out.receipt.terminal_state, {{"output_digest", out.receipt.output_digest}, {"error_code", out.error_code}}));
        return out;
    }
    bool ChildTaskSkillRunner::cancel(std::string_view id)
    {
        std::lock_guard lock(mutex_);
        auto it = active_.find(std::string(id));
        if (it == active_.end())
            return false;
        it->second->cancel();
        return true;
    }
    NestedWorkflowSkillRunner::NestedWorkflowSkillRunner(std::shared_ptr<NestedWorkflowPort> port) : port_(std::move(port))
    {
        if (!port_)
            throw std::invalid_argument("NestedWorkflowPort required");
    }
    ApprovalSkillRunner::ApprovalSkillRunner(approval::ApprovalStore &store) : store_(store) {}
    RunnerResult ApprovalSkillRunner::run(const RunnerRequest &r)
    {
        RunnerResult out;
        out.receipt.invocation_id = r.invocation.invocation_id;
        out.receipt.node_id = r.node.node_id;
        out.receipt.runner = kind();
        out.events = {event(r, 1, RunnerLifecycleState::Admitted), event(r, 2, RunnerLifecycleState::Prepared)};
        const auto id = r.input.value("approval_id", std::string{});
        if (id.empty())
        {
            out.error_code = "approval_id_required";
            out.receipt.terminal_state = RunnerLifecycleState::Failed;
            return out;
        }
        auto request = store_.request(id);
        if (!request)
        {
            out.error_code = "approval_request_not_found";
            out.receipt.terminal_state = RunnerLifecycleState::Failed;
            return out;
        }
        auto decision = store_.latest_decision(id);
        if (!decision)
        {
            out.error_code = "awaiting_approval";
            out.error_message = id;
            out.receipt.terminal_state = RunnerLifecycleState::Waiting;
            out.receipt.checkpoint_ref = "approval:" + id;
            out.events.push_back(event(r, 3, RunnerLifecycleState::Waiting, {{"approval_id", id}, {"checkpoint_ref", out.receipt.checkpoint_ref}}));
            return out;
        }
        const auto request_digest = approval::encode(*request).at("canonical_digest").get<std::string>();
        const auto expected_arguments = r.input.value("arguments_digest", std::string{});
        const auto expected_policy = r.input.value("policy_revision", std::string{});
        if (decision->request_digest != request_digest || (!r.invocation.plan_digest.empty() && decision->plan_digest != r.invocation.plan_digest) ||
            (!expected_arguments.empty() && decision->arguments_digest != expected_arguments) ||
            (!expected_policy.empty() && decision->policy_revision != expected_policy))
        {
            out.error_code = "stale_or_scope_mismatched_approval";
            out.receipt.terminal_state = RunnerLifecycleState::Failed;
            out.events.push_back(event(r, 3, RunnerLifecycleState::Failed, {{"approval_id", id}, {"reason", "binding_mismatch"}}));
            return out;
        }
        if ((!request->expires_at.empty() && utc_now() > request->expires_at) || (!decision->expires_at.empty() && utc_now() > decision->expires_at) ||
            decision->decision == approval::Decision::Expired || decision->decision == approval::Decision::Revoked)
        {
            out.error_code = "approval_expired_or_revoked";
            out.receipt.terminal_state = RunnerLifecycleState::Failed;
            return out;
        }
        out.output = approval::encode(*decision);
        out.ok = decision->decision == approval::Decision::Approved;
        out.error_code = out.ok ? "" : "approval_not_granted";
        out.receipt.terminal_state = out.ok ? RunnerLifecycleState::Succeeded : RunnerLifecycleState::Failed;
        out.receipt.output_digest = contracts::embedded_digest(out.output).value_or("");
        out.events.push_back(event(r, 3, out.receipt.terminal_state, {{"approval_id", id}}));
        return out;
    }
    bool SkillRunnerRegistry::register_runner(std::shared_ptr<SkillRunner> r)
    {
        if (!r || (production_ && r->origin() != RunnerOrigin::Production))
            return false;
        std::lock_guard l(mutex_);
        return runners_.emplace(r->kind(), std::move(r)).second;
    }
    std::shared_ptr<SkillRunner> SkillRunnerRegistry::resolve(SkillRunnerKind k) const
    {
        std::lock_guard l(mutex_);
        auto i = runners_.find(k);
        return i == runners_.end() ? nullptr : i->second;
    }
    std::shared_ptr<SkillRunnerRegistry> build_production_runners(ProductionRunnerDependencies dependencies)
    {
        if (!dependencies.toolbus)
            throw std::invalid_argument("ToolBus required");
        auto runners = std::make_shared<SkillRunnerRegistry>(true);
        for (const auto kind : {SkillRunnerKind::LocalCapability, SkillRunnerKind::SandboxedProcess,
                                SkillRunnerKind::Cli, SkillRunnerKind::Mcp})
            if (!runners->register_runner(std::make_shared<ToolBusSkillRunner>(kind, dependencies.toolbus)))
                throw std::runtime_error("failed to register production runner");
        if (dependencies.child_tasks)
            runners->register_runner(std::make_shared<ChildTaskSkillRunner>(dependencies.child_tasks));
        if (dependencies.nested_workflows)
            runners->register_runner(std::make_shared<NestedWorkflowSkillRunner>(dependencies.nested_workflows));
        if (dependencies.approvals)
            runners->register_runner(std::make_shared<ApprovalSkillRunner>(*dependencies.approvals));
        return runners;
    }
    std::shared_ptr<SkillRunnerRegistry> build_production_toolbus_runners(std::shared_ptr<ToolBus> bus) { return build_production_runners({std::move(bus), {}, {}, nullptr}); }
}
