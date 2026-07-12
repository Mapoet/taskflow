#ifndef AGENT_FRAMEWORK_CHILD_TASK_HPP
#define AGENT_FRAMEWORK_CHILD_TASK_HPP

#include <agent/types.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace agent_framework {

enum class ChildTaskStartMode { Start, Retry, Restart, Resume };
enum class ChildTaskStatus { Pending, Working, Completed, Failed, Cancelled, DeadlineExceeded };

struct ChildTaskPolicy {
    std::size_t max_depth = 8;
    std::size_t max_attempts = 3;
    std::size_t max_iterations = 32;
    std::size_t max_tool_calls = 128;
    std::chrono::milliseconds poll_interval{50};
    std::optional<std::chrono::steady_clock::time_point> deadline;
    std::shared_ptr<std::atomic_bool> cancel_requested;
};

struct ChildTaskUsage {
    std::size_t iterations = 0;
    std::size_t tool_calls = 0;
    std::size_t input_tokens = 0;
    std::size_t output_tokens = 0;
};

struct ChildTaskRequest {
    std::string child_id;
    std::string parent_run_id;
    std::string run_id;
    std::string trace_id;
    std::string idempotency_key;
    std::size_t depth = 0;
    std::size_t attempt = 0;
    std::size_t iteration = 0;
    ChildTaskStartMode mode = ChildTaskStartMode::Start;
    json inputs = json::object();
    ChildTaskPolicy policy;
};

struct ChildTaskResult {
    ChildTaskStatus status = ChildTaskStatus::Failed;
    std::string child_id;
    std::string run_id;
    std::size_t attempt = 0;
    json outputs = json::object();
    std::optional<std::string> error;
    ChildTaskUsage usage;

    bool ok() const { return status == ChildTaskStatus::Completed; }
};

class ChildTaskHandle {
public:
    virtual ~ChildTaskHandle() = default;
    virtual ChildTaskResult wait() = 0;
    virtual void cancel() = 0;
};

class ChildTaskBackend {
public:
    virtual ~ChildTaskBackend() = default;
    virtual std::unique_ptr<ChildTaskHandle> start(ChildTaskRequest request) = 0;
    std::unique_ptr<ChildTaskHandle> submit(ChildTaskRequest request) {
        return start(std::move(request));
    }
    std::unique_ptr<ChildTaskHandle> retry(ChildTaskRequest request) {
        request.mode = ChildTaskStartMode::Retry;
        ++request.attempt;
        return start(std::move(request));
    }
    std::unique_ptr<ChildTaskHandle> restart(ChildTaskRequest request) {
        request.mode = ChildTaskStartMode::Restart;
        ++request.attempt;
        return start(std::move(request));
    }
    std::unique_ptr<ChildTaskHandle> resume(ChildTaskRequest request) {
        request.mode = ChildTaskStartMode::Resume;
        return start(std::move(request));
    }
};

using LocalChildTaskRunner = std::function<ChildTaskResult(const ChildTaskRequest&)>;

class LocalChildTaskBackend final : public ChildTaskBackend {
public:
    explicit LocalChildTaskBackend(LocalChildTaskRunner runner);
    std::unique_ptr<ChildTaskHandle> start(ChildTaskRequest request) override;

private:
    LocalChildTaskRunner runner_;
};

class AgentClient;

class A2AChildTaskBackend final : public ChildTaskBackend {
public:
    A2AChildTaskBackend(std::shared_ptr<AgentClient> client, std::string endpoint);
    std::unique_ptr<ChildTaskHandle> start(ChildTaskRequest request) override;

private:
    std::shared_ptr<AgentClient> client_;
    std::string endpoint_;
};

using LocalSubflowBackend = LocalChildTaskBackend;
using A2ARemoteBackend = A2AChildTaskBackend;

} // namespace agent_framework

#endif
