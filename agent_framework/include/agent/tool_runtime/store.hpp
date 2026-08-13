#pragma once
#include <mutex>
#include "agent/distributed/object_store.hpp"
#include "agent/tool_runtime/types.hpp"
namespace agent_framework::tool_runtime
{
    struct InvocationQuery
    {
        std::string tenant_id, conversation_id, run_id, tool_call_id;
        std::size_t limit{100};
    };
    struct InvocationRetentionPolicy
    {
        std::uint64_t maximum_events{0};
        std::uint64_t minimum_age_ms{0};
        bool dry_run{false};
    };
    struct InvocationRetentionResult
    {
        bool applied{false};
        std::uint64_t first_sequence{0}, last_sequence{0}, events{0};
        std::string archive_digest, error;
    };
    struct StoreResult
    {
        InvocationStoreStatus status{InvocationStoreStatus::Error};
        std::uint64_t revision{0};
        std::string error;
        explicit operator bool() const noexcept { return status == InvocationStoreStatus::Committed; }
    };
    struct InvocationCommit
    {
        LongRunningToolInvocation invocation;
        std::uint64_t expected_revision{0};
        InvocationEvent event;
        std::optional<ProgressCheckpoint> progress;
        std::optional<PartialResultRef> partial;
        std::optional<InvocationReceipt> receipt;
    };
    struct HistoryVerification
    {
        bool valid{false};
        std::uint64_t events{0};
        std::string error;
    };
    class InvocationStore
    {
    public:
        virtual ~InvocationStore() = default;
        virtual StoreResult create(const LongRunningToolInvocation &) = 0;
        virtual std::optional<LongRunningToolInvocation> load(std::string_view) = 0;
        virtual StoreResult commit(InvocationCommit) = 0;
        virtual std::vector<LongRunningToolInvocation> recoverable(std::size_t) = 0;
        virtual std::vector<InvocationEvent> events(std::string_view, std::uint64_t = 0,
                                                    std::size_t = 0) = 0;
        virtual std::uint64_t event_head(std::string_view) = 0;
        virtual std::uint64_t event_retention_floor(std::string_view) = 0;
        virtual std::vector<LongRunningToolInvocation> query(const InvocationQuery &) = 0;
        virtual InvocationRetentionResult apply_retention(
            std::string_view, const InvocationRetentionPolicy &,
            distributed::ObjectStore &) = 0;
        virtual std::vector<PartialResultRef> partial_results(std::string_view) = 0;
        virtual std::optional<ProgressCheckpoint> latest_progress(std::string_view) = 0;
        virtual HistoryVerification verify_history(std::string_view) = 0;
    };
    class SQLiteInvocationStore final : public InvocationStore
    {
    public:
        explicit SQLiteInvocationStore(std::string, int = 3000);
        ~SQLiteInvocationStore();
        StoreResult create(const LongRunningToolInvocation &) override;
        std::optional<LongRunningToolInvocation> load(std::string_view) override;
        StoreResult commit(InvocationCommit) override;
        std::vector<LongRunningToolInvocation> recoverable(std::size_t) override;
        std::vector<InvocationEvent> events(std::string_view, std::uint64_t = 0,
                                            std::size_t = 0) override;
        std::uint64_t event_head(std::string_view) override;
        std::uint64_t event_retention_floor(std::string_view) override;
        std::vector<LongRunningToolInvocation> query(const InvocationQuery &) override;
        InvocationRetentionResult apply_retention(std::string_view,
            const InvocationRetentionPolicy &, distributed::ObjectStore &) override;
        std::vector<PartialResultRef> partial_results(std::string_view) override;
        std::optional<ProgressCheckpoint> latest_progress(std::string_view) override;
        HistoryVerification verify_history(std::string_view) override;

    private:
        void migrate();
        void *db_{nullptr};
        std::mutex mutex_;
    };
}
