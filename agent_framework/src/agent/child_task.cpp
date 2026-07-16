#include <agent/agent/child_task.hpp>

#include <agent/agent_client/agent_client.hpp>

#include <future>
#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace agent_framework {
namespace {

const std::vector<std::string>& grant_values(const SkillPermissionGrant& grant,
                                              std::size_t index) {
    switch (index) {
    case 0: return grant.tools;
    case 1: return grant.network;
    case 2: return grant.environment;
    case 3: return grant.filesystem_read;
    case 4: return grant.filesystem_write;
    default: return grant.secrets;
    }
}

bool cancelled_or_expired(const ChildTaskPolicy& policy, ChildTaskStatus& status) {
    if (policy.cancel_requested && policy.cancel_requested->load(std::memory_order_acquire)) {
        status = ChildTaskStatus::Cancelled;
        return true;
    }
    if (policy.deadline && std::chrono::steady_clock::now() >= *policy.deadline) {
        status = ChildTaskStatus::DeadlineExceeded;
        return true;
    }
    return false;
}

ChildTaskResult enforce_output_budget(ChildTaskResult result,
                                      const ChildTaskPolicy& policy) {
    const std::size_t bytes = result.outputs.dump().size();
    if (bytes <= policy.max_output_bytes) return result;
    result.status = ChildTaskStatus::Failed;
    result.outputs = json::object();
    result.error = "child task output budget exceeded";
    result.error_code = "child_task_output_budget_exceeded";
    return result;
}

void validate_request(const ChildTaskRequest& request) {
    if (request.child_id.empty() || request.run_id.empty()) {
        throw std::invalid_argument("child task requires child_id and run_id");
    }
    if (request.depth > request.policy.max_depth) {
        throw std::invalid_argument("child task depth limit exceeded");
    }
    if (request.attempt >= request.policy.max_attempts) {
        throw std::invalid_argument("child task attempt limit exceeded");
    }
    if (request.inputs.dump().size() > request.policy.max_input_bytes) {
        throw std::invalid_argument("child task input budget exceeded");
    }
}

class LocalHandle final : public ChildTaskHandle {
public:
    LocalHandle(LocalChildTaskRunner runner, ChildTaskRequest request)
        : cancel_(request.policy.cancel_requested ? request.policy.cancel_requested
                                                  : std::make_shared<std::atomic_bool>(false)) {
        request.policy.cancel_requested = cancel_;
        future_ = std::async(std::launch::async, [runner = std::move(runner), request = std::move(request)] {
            ChildTaskStatus terminal;
            if (cancelled_or_expired(request.policy, terminal)) {
                ChildTaskResult r;
                r.status = terminal;
                r.child_id = request.child_id;
                r.run_id = request.run_id;
                r.attempt = request.attempt;
                return r;
            }
            return enforce_output_budget(runner(request), request.policy);
        });
    }

    ChildTaskResult wait() override { return future_.get(); }
    void cancel() override { cancel_->store(true, std::memory_order_release); }

private:
    std::shared_ptr<std::atomic_bool> cancel_;
    std::future<ChildTaskResult> future_;
};

class A2AHandle final : public ChildTaskHandle {
public:
    A2AHandle(std::shared_ptr<AgentClient> client, std::string endpoint, ChildTaskRequest request)
        : client_(std::move(client)), endpoint_(std::move(endpoint)), request_(std::move(request)),
          cancel_(request_.policy.cancel_requested ? request_.policy.cancel_requested
                                                   : std::make_shared<std::atomic_bool>(false)) {
        request_.policy.cancel_requested = cancel_;
    }

    ChildTaskResult wait() override {
        ChildTaskResult out;
        out.child_id = request_.child_id;
        out.run_id = request_.run_id;
        out.attempt = request_.attempt;
        ChildTaskStatus early;
        if (cancelled_or_expired(request_.policy, early)) {
            out.status = early;
            return out;
        }

        AgentMessage message;
        message.role = AgentMessage::Role::USER;
        AgentPart part;
        part.type = AgentPart::Type::TEXT;
        part.text = request_.inputs.dump();
        message.parts.push_back(std::move(part));
        json metadata = child_task_request_metadata(request_);
        AgentTask task = client_->send_task(endpoint_, message, request_.trace_id, metadata).get();
        {
            std::lock_guard<std::mutex> lock(remote_mutex_);
            remote_task_id_ = task.task_id;
        }
        for (;;) {
            if (cancelled_or_expired(request_.policy, early)) {
                (void)client_->cancel_task(endpoint_, task.task_id).get();
                out.status = early;
                return enforce_output_budget(std::move(out), request_.policy);
            }
            task = client_->get_task(endpoint_, task.task_id).get();
            if (task.status == AgentTaskStatus::COMPLETED) {
                if (task.metadata.contains("childResult")) {
                    out = child_task_result_from_json(task.metadata.at("childResult"));
                    if (out.child_id.empty()) out.child_id = request_.child_id;
                    if (out.run_id.empty()) out.run_id = request_.run_id;
                } else {
                    out.status = ChildTaskStatus::Completed;
                    out.outputs = task.to_json();
                }
                return enforce_output_budget(std::move(out), request_.policy);
            }
            if (task.status == AgentTaskStatus::FAILED || task.status == AgentTaskStatus::CANCELLED) {
                out.status = task.status == AgentTaskStatus::CANCELLED ? ChildTaskStatus::Cancelled
                                                                      : ChildTaskStatus::Failed;
                out.outputs = task.to_json();
                return enforce_output_budget(std::move(out), request_.policy);
            }
            std::this_thread::sleep_for(request_.policy.poll_interval);
        }
    }

    void cancel() override {
        cancel_->store(true, std::memory_order_release);
        std::string task_id;
        {
            std::lock_guard<std::mutex> lock(remote_mutex_);
            task_id = remote_task_id_;
        }
        if (!task_id.empty()) {
            (void)client_->cancel_task(endpoint_, task_id).get();
        }
    }

private:
    std::shared_ptr<AgentClient> client_;
    std::string endpoint_;
    ChildTaskRequest request_;
    std::shared_ptr<std::atomic_bool> cancel_;
    std::mutex remote_mutex_;
    std::string remote_task_id_;
};

} // namespace

LocalChildTaskBackend::LocalChildTaskBackend(LocalChildTaskRunner runner) : runner_(std::move(runner)) {
    if (!runner_) {
        throw std::invalid_argument("LocalChildTaskBackend requires runner");
    }
}

std::unique_ptr<ChildTaskHandle> LocalChildTaskBackend::start(ChildTaskRequest request) {
    validate_request(request);
    return std::make_unique<LocalHandle>(runner_, std::move(request));
}

A2AChildTaskBackend::A2AChildTaskBackend(std::shared_ptr<AgentClient> client, std::string endpoint)
    : client_(std::move(client)), endpoint_(std::move(endpoint)) {
    if (!client_) {
        throw std::invalid_argument("A2AChildTaskBackend requires client");
    }
}

std::unique_ptr<ChildTaskHandle> A2AChildTaskBackend::start(ChildTaskRequest request) {
    validate_request(request);
    return std::make_unique<A2AHandle>(client_, endpoint_, std::move(request));
}

const char* child_task_start_mode_cstr(ChildTaskStartMode mode) noexcept {
    switch (mode) {
    case ChildTaskStartMode::Start: return "start";
    case ChildTaskStartMode::Retry: return "retry";
    case ChildTaskStartMode::Restart: return "restart";
    case ChildTaskStartMode::Resume: return "resume";
    }
    return "start";
}

const char* child_task_status_cstr(ChildTaskStatus status) noexcept {
    switch (status) {
    case ChildTaskStatus::Pending: return "pending";
    case ChildTaskStatus::Working: return "working";
    case ChildTaskStatus::Completed: return "completed";
    case ChildTaskStatus::Failed: return "failed";
    case ChildTaskStatus::Cancelled: return "cancelled";
    case ChildTaskStatus::DeadlineExceeded: return "deadline_exceeded";
    }
    return "failed";
}

json child_task_result_to_json(const ChildTaskResult& result) {
    json out = {{"status", child_task_status_cstr(result.status)},
                {"childId", result.child_id}, {"runId", result.run_id},
                {"attempt", result.attempt}, {"outputs", result.outputs},
                {"errorCode", result.error_code}, {"events", result.events},
                {"checkpoint", result.checkpoint},
                {"usage", {{"iterations", result.usage.iterations},
                            {"toolCalls", result.usage.tool_calls},
                            {"inputTokens", result.usage.input_tokens},
                            {"outputTokens", result.usage.output_tokens}}}};
    if (result.error) out["error"] = *result.error;
    return out;
}

ChildTaskResult child_task_result_from_json(const json& value) {
    ChildTaskResult out;
    const std::string status = value.value("status", "failed");
    if (status == "pending") out.status = ChildTaskStatus::Pending;
    else if (status == "working") out.status = ChildTaskStatus::Working;
    else if (status == "completed") out.status = ChildTaskStatus::Completed;
    else if (status == "cancelled") out.status = ChildTaskStatus::Cancelled;
    else if (status == "deadline_exceeded") out.status = ChildTaskStatus::DeadlineExceeded;
    else out.status = ChildTaskStatus::Failed;
    out.child_id = value.value("childId", "");
    out.run_id = value.value("runId", "");
    out.attempt = value.value("attempt", 0U);
    out.outputs = value.value("outputs", json::object());
    if (value.contains("error") && value.at("error").is_string())
        out.error = value.at("error").get<std::string>();
    out.error_code = value.value("errorCode", "");
    out.events = value.value("events", json::array());
    out.checkpoint = value.value("checkpoint", json::object());
    const json usage = value.value("usage", json::object());
    out.usage.iterations = usage.value("iterations", 0U);
    out.usage.tool_calls = usage.value("toolCalls", 0U);
    out.usage.input_tokens = usage.value("inputTokens", 0U);
    out.usage.output_tokens = usage.value("outputTokens", 0U);
    return out;
}

json child_task_request_metadata(const ChildTaskRequest& request) {
    json metadata = {{"childId", request.child_id}, {"parentRunId", request.parent_run_id},
            {"runId", request.run_id}, {"traceId", request.trace_id},
            {"idempotencyKey", request.idempotency_key}, {"depth", request.depth},
            {"attempt", request.attempt}, {"iteration", request.iteration},
            {"startMode", child_task_start_mode_cstr(request.mode)},
            {"checkpoint", request.checkpoint},
            {"permissions", {{"tools", request.grants.tools},
                              {"network", request.grants.network},
                              {"environment", request.grants.environment},
                              {"filesystemRead", request.grants.filesystem_read},
                              {"filesystemWrite", request.grants.filesystem_write},
                              {"secrets", request.grants.secrets}}},
            {"budget", {{"maxInputBytes", request.policy.max_input_bytes},
                        {"maxOutputBytes", request.policy.max_output_bytes},
                        {"maxIterations", request.policy.max_iterations},
                        {"maxToolCalls", request.policy.max_tool_calls}}}};
    if (request.policy.deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            *request.policy.deadline - std::chrono::steady_clock::now());
        metadata["deadlineRemainingMs"] = std::max<std::int64_t>(0, remaining.count());
    }
    return metadata;
}

bool child_task_grants_are_narrower(const SkillPermissionGrant& parent,
                                    const SkillPermissionGrant& child,
                                    std::string* reason) {
    static constexpr const char* names[] = {
        "tools", "network", "environment", "filesystem_read",
        "filesystem_write", "secrets"};
    for (std::size_t i = 0; i < 6; ++i) {
        const auto& parent_values = grant_values(parent, i);
        for (const auto& value : grant_values(child, i)) {
            if (std::find(parent_values.begin(), parent_values.end(), value) ==
                parent_values.end()) {
                if (reason) *reason = std::string(names[i]) + " grant escalates: " + value;
                return false;
            }
        }
    }
    return true;
}

} // namespace agent_framework
