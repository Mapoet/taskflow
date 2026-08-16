#pragma once

#include "agent/conversation/task_control_service.hpp"
#include "agent/conversation/task_orchestrator.hpp"

namespace agent_framework::conversation {

enum class TaskCommandKind { Start, Status, Output, Continue, Amend, Suspend,
                             Cancel, Attach, Replan };

struct TaskCommandRequest {
    TaskCommandKind command{TaskCommandKind::Status};
    TurnRequest turn;
    std::string reason;
    std::int64_t now_ms{0};
};

struct TaskCommandResponse {
    bool ok{false};
    bool read_only{false};
    std::uint64_t task_revision{0};
    nlohmann::json payload=nlohmann::json::object();
    std::string error;
};

class TaskCommandService {
public:
    TaskCommandService(TaskRegistry& tasks, TaskControlService* controls=nullptr)
        : tasks_(tasks), orchestrator_(tasks), controls_(controls) {}
    TaskCommandResponse execute(const TaskCommandRequest&);
private:
    TaskRegistry& tasks_;
    TaskOrchestrator orchestrator_;
    TaskControlService* controls_{nullptr};
};

std::string_view name(TaskCommandKind) noexcept;

} // namespace agent_framework::conversation
