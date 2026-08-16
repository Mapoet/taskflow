#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "agent/conversation/task_registry.hpp"
#include "agent/harness/types.hpp"
#include "agent/run/types.hpp"

namespace agent_framework::recovery {

enum class CoordinationCommand {
    None,
    ContinueExecution,
    AwaitInput,
    AwaitApproval,
    AwaitExternal,
    VerifyCompletion,
    CloseVerified,
    Fail,
    Cancel,
    ManualReview
};

struct CorrelatedStateEvent {
    conversation::ConversationIdentity identity;
    std::string task_id;
    std::string turn_id;
    std::string run_id;
    std::string harness_id;
    std::uint64_t task_revision{0};
    conversation::TurnPhase turn_phase{conversation::TurnPhase::Pending};
    run::RunState run_state{run::RunState::Created};
    harness::HarnessState harness_state{harness::HarnessState::Running};
    bool invocation_active{false};
    bool pending_effect{false};
    bool unknown_effect{false};
    bool closure_verified{false};
    std::string source_event_id;
};

struct TaskCoordinationDecision {
    CoordinationCommand command{CoordinationCommand::None};
    conversation::TaskLifecycleState task_state{
        conversation::TaskLifecycleState::Active};
    std::string closure_state{"running"};
    std::string reason_code;
    bool terminal{false};
    std::string digest;
};

enum class TaskCoordinationCommandState { Pending, Applied, Conflict };

struct DurableTaskCoordinationCommand {
    std::string command_id;
    CorrelatedStateEvent event;
    TaskCoordinationDecision decision;
    TaskCoordinationCommandState state{TaskCoordinationCommandState::Pending};
    std::uint64_t journal_revision{1};
    std::string diagnostic;
};

class SQLiteTaskCoordinationJournal {
public:
    explicit SQLiteTaskCoordinationJournal(std::string path);
    ~SQLiteTaskCoordinationJournal();
    SQLiteTaskCoordinationJournal(const SQLiteTaskCoordinationJournal&) = delete;
    SQLiteTaskCoordinationJournal& operator=(const SQLiteTaskCoordinationJournal&) = delete;
    bool submit(const DurableTaskCoordinationCommand&, std::string* error = nullptr);
    std::optional<DurableTaskCoordinationCommand> load(std::string_view command_id);
    std::vector<DurableTaskCoordinationCommand> pending(std::size_t limit);
    bool transition(std::string_view command_id, std::uint64_t expected_revision,
                    TaskCoordinationCommandState, std::string_view diagnostic,
                    std::string* error = nullptr);
private:
    void* db_{nullptr};
    std::mutex mutex_;
};

// The sole deterministic authority that translates correlated subsystem state
// into a Task lifecycle command. It never treats execution completion as proof
// that semantic acceptance criteria were verified.
class TaskStateCoordinator {
public:
    TaskCoordinationDecision observe(const CorrelatedStateEvent&) const;
};

class DurableTaskStateCoordinator {
public:
    DurableTaskStateCoordinator(conversation::TaskRegistry& tasks,
                                SQLiteTaskCoordinationJournal& journal)
        : tasks_(tasks), journal_(journal) {}
    bool publish(const CorrelatedStateEvent&, const TaskCoordinationDecision&,
                 std::string* error = nullptr);
    bool reconcile(std::string_view command_id, std::string* error = nullptr);
    std::size_t reconcile_pending(std::size_t limit);
private:
    conversation::TaskRegistry& tasks_;
    SQLiteTaskCoordinationJournal& journal_;
};

std::string_view name(CoordinationCommand) noexcept;
std::string_view name(TaskCoordinationCommandState) noexcept;

}  // namespace agent_framework::recovery
