#pragma once
#include <mutex>
#include "agent/conversation/types.hpp"

namespace agent_framework::distributed { class ObjectStore; }

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

    struct EventRetentionPolicy
    {
        std::size_t keep_last_events{1000};
        std::size_t maximum_archive_events{1000};
        bool dry_run{false};
    };

    struct EventArchiveRecord
    {
        ConversationIdentity identity;
        std::string archive_id, state, object_digest, media_type;
        std::uint64_t first_sequence{0}, last_sequence{0}, event_count{0}, object_size{0};
        std::string first_event_digest, last_event_digest, previous_archive_digest;
    };

    struct EventCompactionResult
    {
        bool ok{false}, changed{false}, dry_run{false};
        std::uint64_t first_sequence{0}, last_sequence{0}, event_count{0};
        std::string archive_id, object_digest, error;
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
        virtual std::vector<TurnCheckpoint> list_turns(
            const ConversationIdentity &, bool nonterminal_only = false,
            std::size_t limit = 0) = 0;
        virtual bool append_event(RuntimeEventEnvelope, std::string * = nullptr) = 0;
        virtual std::vector<RuntimeEventEnvelope> events(const ConversationIdentity &,
                                                         std::uint64_t after = 0,
                                                         std::size_t limit = 0) = 0;
        virtual std::uint64_t last_event_sequence(const ConversationIdentity &) = 0;
        virtual std::uint64_t event_retention_floor(const ConversationIdentity &) = 0;
        virtual EventCompactionResult compact_events(
            const ConversationIdentity &, const EventRetentionPolicy &,
            distributed::ObjectStore &) = 0;
        virtual std::vector<EventArchiveRecord> event_archives(
            const ConversationIdentity &) = 0;
        virtual bool verify_event_archive(const EventArchiveRecord &,
                                          distributed::ObjectStore &,
                                          std::string * = nullptr) = 0;
        virtual bool append_boundary(CompactBoundaryRecord, std::string * = nullptr) = 0;
        virtual std::optional<CompactBoundaryRecord> latest_boundary(
            const ConversationIdentity &) = 0;
        virtual bool commit(ConversationCommit &, std::string * = nullptr) = 0;
        virtual std::vector<ConversationInput> inputs(const ConversationIdentity &,
                                                      InputState) = 0;
        virtual std::optional<QueuedTurnClaim> consume_next_queued_input(
            const ConversationIdentity &, std::string * = nullptr) = 0;
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
        std::vector<TurnCheckpoint> list_turns(
            const ConversationIdentity &, bool nonterminal_only = false,
            std::size_t limit = 0) override;
        bool append_event(RuntimeEventEnvelope, std::string *) override;
        std::vector<RuntimeEventEnvelope> events(
            const ConversationIdentity &, std::uint64_t after = 0,
            std::size_t limit = 0) override;
        std::uint64_t last_event_sequence(const ConversationIdentity &) override;
        std::uint64_t event_retention_floor(const ConversationIdentity &) override;
        EventCompactionResult compact_events(const ConversationIdentity &,
                                             const EventRetentionPolicy &,
                                             distributed::ObjectStore &) override;
        std::vector<EventArchiveRecord> event_archives(const ConversationIdentity &) override;
        bool verify_event_archive(const EventArchiveRecord &, distributed::ObjectStore &,
                                  std::string * = nullptr) override;
        bool append_boundary(CompactBoundaryRecord, std::string *) override;
        std::optional<CompactBoundaryRecord> latest_boundary(const ConversationIdentity &) override;
        bool commit(ConversationCommit &, std::string * = nullptr) override;
        std::vector<ConversationInput> inputs(const ConversationIdentity &, InputState) override;
        std::optional<QueuedTurnClaim> consume_next_queued_input(
            const ConversationIdentity &, std::string * = nullptr) override;

    private:
        void *db_{nullptr};
        std::mutex mutex_;
    };

} // namespace agent_framework::conversation
