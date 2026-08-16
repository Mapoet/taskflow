#pragma once

#include "agent/conversation/task_control_service.hpp"
#include "agent/conversation/task_orchestrator.hpp"

#include <optional>
#include <vector>

namespace agent_framework::conversation {

enum class TaskCommandKind { Start, Status, Output, Continue, Amend, Suspend,
                             Cancel, Attach, Replan };

struct TaskCommandPrincipal {
    std::string actor_id;
    ConversationIdentity identity;
    std::vector<std::string> scopes;
    bool authenticated{false};
};

struct TaskCommandAction {
    TaskCommandKind command{TaskCommandKind::Status};
    std::string required_scope;
    bool enabled{false};
    std::string reason;
};

class TaskCommandPolicy {
public:
    std::string authorize(const TaskCommandPrincipal&, const TurnRequest&,
                          TaskCommandKind) const;
    std::vector<TaskCommandAction> actions(
        const TaskCommandPrincipal&, const TurnRequest&,
        const std::optional<PersistentTask>&) const;
};

struct TaskCommandRequest {
    TaskCommandKind command{TaskCommandKind::Status};
    TurnRequest turn;
    std::string reason;
    std::int64_t now_ms{0};
    std::optional<std::uint64_t> expected_task_revision;
    std::optional<TaskCommandPrincipal> principal;
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
    TaskCommandService(TaskRegistry& tasks, TaskControlService* controls=nullptr,
                       const TaskCommandPolicy* policy=nullptr)
        : tasks_(tasks), orchestrator_(tasks), controls_(controls), policy_(policy) {}
    TaskCommandResponse execute(const TaskCommandRequest&);
private:
    TaskRegistry& tasks_;
    TaskOrchestrator orchestrator_;
    TaskControlService* controls_{nullptr};
    const TaskCommandPolicy* policy_{nullptr};
};

std::string_view name(TaskCommandKind) noexcept;

} // namespace agent_framework::conversation
