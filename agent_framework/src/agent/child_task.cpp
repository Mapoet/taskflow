#include <agent/child_task.hpp>

#include <agent/agent_client.hpp>

#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace agent_framework {
namespace {

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
            return runner(request);
        });
    }

    ChildTaskResult wait() override { return future_.get(); }
    void cancel() override { cancel_->store(true, std::memory_order_release); }

private:
    std::shared_ptr<std::atomic_bool> cancel_;
    std::future<ChildTaskResult> future_;
};

class ImmediateHandle final : public ChildTaskHandle {
public:
    explicit ImmediateHandle(ChildTaskResult result) : result_(std::move(result)) {}
    ChildTaskResult wait() override { return result_; }
    void cancel() override {
        if (!result_.ok()) result_.status = ChildTaskStatus::Cancelled;
    }
private:
    ChildTaskResult result_;
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
        json metadata = {{"childId", request_.child_id}, {"parentRunId", request_.parent_run_id},
                         {"runId", request_.run_id}, {"traceId", request_.trace_id},
                         {"idempotencyKey", request_.idempotency_key},
                         {"depth", request_.depth}, {"attempt", request_.attempt},
                         {"iteration", request_.iteration}};
        AgentTask task = client_->send_task(endpoint_, message, request_.trace_id, metadata).get();
        {
            std::lock_guard<std::mutex> lock(remote_mutex_);
            remote_task_id_ = task.task_id;
        }
        for (;;) {
            if (cancelled_or_expired(request_.policy, early)) {
                (void)client_->cancel_task(endpoint_, task.task_id).get();
                out.status = early;
                return out;
            }
            task = client_->get_task(endpoint_, task.task_id).get();
            if (task.status == AgentTaskStatus::COMPLETED) {
                out.status = ChildTaskStatus::Completed;
                out.outputs = task.to_json();
                return out;
            }
            if (task.status == AgentTaskStatus::FAILED || task.status == AgentTaskStatus::CANCELLED) {
                out.status = task.status == AgentTaskStatus::CANCELLED ? ChildTaskStatus::Cancelled
                                                                      : ChildTaskStatus::Failed;
                out.outputs = task.to_json();
                return out;
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
    if (request.mode == ChildTaskStartMode::Resume) {
        ChildTaskResult result;
        result.status = ChildTaskStatus::Failed;
        result.child_id = request.child_id;
        result.run_id = request.run_id;
        result.attempt = request.attempt;
        result.error = "unsupported_resume";
        return std::make_unique<ImmediateHandle>(std::move(result));
    }
    return std::make_unique<A2AHandle>(client_, endpoint_, std::move(request));
}

} // namespace agent_framework
