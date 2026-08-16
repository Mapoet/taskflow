#pragma once

#include <cstdint>
#include <optional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/conversation/types.hpp"

namespace agent_framework::conversation {

enum class TaskLifecycleState {
    Active,
    AwaitingInput,
    AwaitingApproval,
    Suspended,
    Closing,
    Closed,
    Failed,
    Cancelled
};

enum class TaskInputIntent {
    InitialRequest,
    ContinueTask,
    AmendRequirements,
    StatusQuery,
    CancelTask,
    SuspendTask,
    ReplanTask,
    StartNewTask
};

struct PersistentTask {
    ConversationIdentity identity;
    std::string task_id;
    std::string root_turn_id;
    std::string current_turn_id;
    std::string current_run_id;
    std::string parent_task_id;
    TaskLifecycleState state{TaskLifecycleState::Active};
    std::string closure_state{"running"};
    std::uint64_t revision{1};
    std::uint64_t requirement_revision{1};
    std::uint64_t plan_revision{0};
    std::string created_at;
    std::string updated_at;
    std::string digest;
};

struct TaskRequirementRevision {
    ConversationIdentity identity;
    std::string task_id;
    std::uint64_t revision{1};
    TaskInputIntent intent{TaskInputIntent::InitialRequest};
    std::string turn_id;
    std::string content;
    std::string previous_digest;
    std::string digest;
    std::string created_at;
};

struct TurnTaskLink {
    ConversationIdentity identity;
    std::string turn_id;
    std::string task_id;
    std::string run_id;
    std::uint64_t requirement_revision{0};
    TaskInputIntent intent{TaskInputIntent::InitialRequest};
    std::string created_at;
};

struct TaskRunLink {
    ConversationIdentity identity;
    std::string task_id;
    std::string run_id;
    std::uint64_t requirement_revision{0};
    std::uint64_t plan_revision{0};
    std::string state{"active"};
    std::string created_at;
    std::string updated_at;
    std::string plan_digest;
    std::string task_contract_digest;
};

struct TaskMutationResult {
    bool ok{false};
    std::uint64_t revision{0};
    std::string error;
};

class TaskRegistry {
public:
    virtual ~TaskRegistry() = default;
    virtual TaskMutationResult create(PersistentTask,
                                      TaskRequirementRevision,
                                      TurnTaskLink) = 0;
    virtual std::optional<PersistentTask> load(
        const ConversationIdentity&, std::string_view task_id) = 0;
    virtual std::optional<PersistentTask> active(
        const ConversationIdentity&) = 0;
    virtual TaskMutationResult append_requirement(
        const TaskRequirementRevision&, const TurnTaskLink&,
        std::uint64_t expected_task_revision,
        std::string_view run_id) = 0;
    virtual TaskMutationResult attach_turn(const TurnTaskLink&,
                                            std::uint64_t expected_task_revision) = 0;
    virtual TaskMutationResult bind_run(const TaskRunLink&,
                                         std::uint64_t expected_task_revision) = 0;
    virtual TaskMutationResult bind_plan(const TaskRunLink&,
                                         std::uint64_t expected_task_revision) = 0;
    virtual TaskMutationResult transition(
        const ConversationIdentity&, std::string_view task_id,
        std::uint64_t expected_revision, TaskLifecycleState,
        std::string_view closure_state) = 0;
    virtual std::optional<TurnTaskLink> link_for_turn(
        const ConversationIdentity&, std::string_view turn_id) = 0;
    virtual std::vector<TaskRequirementRevision> requirements(
        const ConversationIdentity&, std::string_view task_id) = 0;
    virtual std::vector<TaskRunLink> runs(
        const ConversationIdentity&, std::string_view task_id) = 0;
};

class SQLiteTaskRegistry final : public TaskRegistry {
public:
    explicit SQLiteTaskRegistry(std::string database_path);
    ~SQLiteTaskRegistry() override;
    SQLiteTaskRegistry(const SQLiteTaskRegistry&) = delete;
    SQLiteTaskRegistry& operator=(const SQLiteTaskRegistry&) = delete;

    TaskMutationResult create(PersistentTask, TaskRequirementRevision,
                              TurnTaskLink) override;
    std::optional<PersistentTask> load(
        const ConversationIdentity&, std::string_view task_id) override;
    std::optional<PersistentTask> active(const ConversationIdentity&) override;
    TaskMutationResult append_requirement(
        const TaskRequirementRevision&, const TurnTaskLink&,
        std::uint64_t expected_task_revision,
        std::string_view run_id) override;
    TaskMutationResult attach_turn(const TurnTaskLink&,
                                    std::uint64_t expected_task_revision) override;
    TaskMutationResult bind_run(const TaskRunLink&,
                                 std::uint64_t expected_task_revision) override;
    TaskMutationResult bind_plan(const TaskRunLink&,
                                 std::uint64_t expected_task_revision) override;
    TaskMutationResult transition(
        const ConversationIdentity&, std::string_view task_id,
        std::uint64_t expected_revision, TaskLifecycleState,
        std::string_view closure_state) override;
    std::optional<TurnTaskLink> link_for_turn(
        const ConversationIdentity&, std::string_view turn_id) override;
    std::vector<TaskRequirementRevision> requirements(
        const ConversationIdentity&, std::string_view task_id) override;
    std::vector<TaskRunLink> runs(
        const ConversationIdentity&, std::string_view task_id) override;

private:
    void migrate();
    void* db_{nullptr};
    std::string path_;
    std::mutex mutex_;
};

std::string_view name(TaskLifecycleState);
std::string_view name(TaskInputIntent);
std::optional<TaskLifecycleState> task_lifecycle_state(std::string_view);
std::optional<TaskInputIntent> task_input_intent(std::string_view);
TaskInputIntent classify_task_input(std::string_view input, bool has_active_task);
nlohmann::json encode(const PersistentTask&);
nlohmann::json encode(const TaskRequirementRevision&);

}  // namespace agent_framework::conversation
