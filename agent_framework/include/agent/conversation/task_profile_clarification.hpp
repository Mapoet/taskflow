#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/conversation/task_classifier.hpp"
#include "agent/conversation/task_registry.hpp"

namespace agent_framework::conversation {

enum class ProfileClarificationState {
    Pending,
    Confirmed,
    Exhausted,
    Expired,
    Cancelled
};

struct TaskProfileClarification {
    ConversationIdentity identity;
    std::string clarification_id;
    std::string task_id;
    std::string decision_id;
    std::string turn_id;
    std::string run_id;
    TaskInputIntent task_intent{TaskInputIntent::InitialRequest};
    TaskExecutionProfile recommended_profile{TaskExecutionProfile::Conversation};
    std::optional<TaskExecutionProfile> selected_profile;
    std::string question;
    std::vector<TaskClarificationOption> options;
    std::vector<std::string> allowed_tokens;
    std::uint32_t attempt_count{0};
    std::uint32_t max_attempts{3};
    std::uint64_t revision{1};
    std::uint64_t expires_at_ms{0};
    ProfileClarificationState state{ProfileClarificationState::Pending};
    std::string created_at;
    std::string updated_at;
};

struct ProfileConfirmation {
    std::optional<TaskExecutionProfile> profile;
    std::string error;
    explicit operator bool() const noexcept { return profile.has_value() && error.empty(); }
};

struct ClarificationMutationResult {
    bool ok{false};
    std::uint64_t revision{0};
    ProfileClarificationState state{ProfileClarificationState::Pending};
    std::string error;
};

ProfileConfirmation parse_profile_confirmation(std::string_view);
std::string_view name(ProfileClarificationState);

class TaskProfileClarificationStore {
public:
    virtual ~TaskProfileClarificationStore() = default;
    virtual ClarificationMutationResult create(TaskProfileClarification) = 0;
    virtual std::optional<TaskProfileClarification> load(
        const ConversationIdentity&, std::string_view clarification_id) = 0;
    virtual std::optional<TaskProfileClarification> pending(
        const ConversationIdentity&) = 0;
    virtual std::optional<TaskProfileClarification> latest(
        const ConversationIdentity&) = 0;
    virtual ClarificationMutationResult answer(
        const ConversationIdentity&, std::string_view clarification_id,
        std::uint64_t expected_revision, std::string_view answer,
        std::uint64_t now_ms) = 0;
    virtual ClarificationMutationResult cancel(
        const ConversationIdentity&, std::string_view clarification_id,
        std::uint64_t expected_revision) = 0;
};

class SQLiteTaskProfileClarificationStore final
    : public TaskProfileClarificationStore {
public:
    explicit SQLiteTaskProfileClarificationStore(std::string database_path);
    ~SQLiteTaskProfileClarificationStore() override;
    SQLiteTaskProfileClarificationStore(
        const SQLiteTaskProfileClarificationStore&) = delete;
    SQLiteTaskProfileClarificationStore& operator=(
        const SQLiteTaskProfileClarificationStore&) = delete;

    ClarificationMutationResult create(TaskProfileClarification) override;
    std::optional<TaskProfileClarification> load(
        const ConversationIdentity&, std::string_view clarification_id) override;
    std::optional<TaskProfileClarification> pending(
        const ConversationIdentity&) override;
    std::optional<TaskProfileClarification> latest(
        const ConversationIdentity&) override;
    ClarificationMutationResult answer(
        const ConversationIdentity&, std::string_view clarification_id,
        std::uint64_t expected_revision, std::string_view answer,
        std::uint64_t now_ms) override;
    ClarificationMutationResult cancel(
        const ConversationIdentity&, std::string_view clarification_id,
        std::uint64_t expected_revision) override;

private:
    void migrate();
    void* db_{nullptr};
    std::string path_;
    std::mutex mutex_;
};

}  // namespace agent_framework::conversation
