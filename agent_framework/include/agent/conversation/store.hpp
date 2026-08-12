#pragma once
#include <mutex>
#include "agent/conversation/types.hpp"

namespace agent_framework::conversation
{
    struct ConversationCommit
    {
        TurnCheckpoint checkpoint;
        std::uint64_t expected_turn_revision{0};
        std::vector<ConversationMessage> messages;
        std::vector<RuntimeEventEnvelope> durable_events;
        std::vector<ConversationInput> inputs;
    };

    class ConversationStore
    {
    public:
        virtual ~ConversationStore() = default;
        virtual bool append_message(ConversationMessage, std::string * = nullptr) = 0;
        virtual std::vector<ConversationMessage> messages(const ConversationIdentity &) = 0;
        virtual bool commit_turn(TurnCheckpoint, std::uint64_t expected_revision,
                                 std::string * = nullptr) = 0;
        virtual std::optional<TurnCheckpoint> load_turn(const ConversationIdentity &,
                                                        std::string_view turn_id) = 0;
        virtual bool append_event(RuntimeEventEnvelope, std::string * = nullptr) = 0;
        virtual std::vector<RuntimeEventEnvelope> events(const ConversationIdentity &,
                                                         std::uint64_t after = 0) = 0;
        virtual std::uint64_t last_event_sequence(const ConversationIdentity &) = 0;
        virtual bool append_boundary(CompactBoundaryRecord, std::string * = nullptr) = 0;
        virtual std::optional<CompactBoundaryRecord> latest_boundary(
            const ConversationIdentity &) = 0;
        virtual bool commit(ConversationCommit &, std::string * = nullptr) = 0;
        virtual std::vector<ConversationInput> inputs(const ConversationIdentity &,
                                                      InputState) = 0;
    };

    class SQLiteConversationStore final : public ConversationStore
    {
    public:
        explicit SQLiteConversationStore(std::string path);
        ~SQLiteConversationStore();
        bool append_message(ConversationMessage, std::string *) override;
        std::vector<ConversationMessage> messages(const ConversationIdentity &) override;
        bool commit_turn(TurnCheckpoint, std::uint64_t, std::string *) override;
        std::optional<TurnCheckpoint> load_turn(const ConversationIdentity &, std::string_view) override;
        bool append_event(RuntimeEventEnvelope, std::string *) override;
        std::vector<RuntimeEventEnvelope> events(
            const ConversationIdentity &, std::uint64_t after = 0) override;
        std::uint64_t last_event_sequence(const ConversationIdentity &) override;
        bool append_boundary(CompactBoundaryRecord, std::string *) override;
        std::optional<CompactBoundaryRecord> latest_boundary(const ConversationIdentity &) override;
        bool commit(ConversationCommit &, std::string * = nullptr) override;
        std::vector<ConversationInput> inputs(const ConversationIdentity &, InputState) override;

    private:
        void *db_{nullptr};
        std::mutex mutex_;
    };

} // namespace agent_framework::conversation
