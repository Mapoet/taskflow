#pragma once
#include <mutex>
#include "agent/tool_runtime/types.hpp"
namespace agent_framework::tool_runtime
{
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
        virtual std::vector<InvocationEvent> events(std::string_view, std::uint64_t = 0) = 0;
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
        std::vector<InvocationEvent> events(std::string_view, std::uint64_t = 0) override;
        std::vector<PartialResultRef> partial_results(std::string_view) override;
        std::optional<ProgressCheckpoint> latest_progress(std::string_view) override;
        HistoryVerification verify_history(std::string_view) override;

    private:
        void *db_{nullptr};
        std::mutex mutex_;
    };
}
