#pragma once
#include "agent/tool_runtime/store.hpp"
#include "agent/distributed/durable_queue.hpp"
#include "agent/tool_runtime/execution_adapter.hpp"
#include "agent/tool_runtime/incremental_result_store.hpp"
namespace agent_framework::tool_runtime
{
    struct WorkerIdentity
    {
        std::string worker_id, instance_id;
        std::uint64_t generation{0};
    };
    struct ClaimedInvocation
    {
        LongRunningToolInvocation invocation;
        distributed::Lease queue_lease;
    };
    class LeaseWorkerRuntime
    {
    public:
        LeaseWorkerRuntime(InvocationStore &, distributed::SQLiteDurableQueue &, distributed::SQLiteWorkerRegistry &, WorkerIdentity, std::int64_t lease_ms);
        StoreResult enqueue(LongRunningToolInvocation);
        std::optional<ClaimedInvocation> claim(std::string_view tenant, std::int64_t now_ms, std::string *error = nullptr);
        bool renew(const ClaimedInvocation &, std::int64_t now_ms, std::string *error = nullptr);
        bool complete(ClaimedInvocation &, InvocationReceipt, std::int64_t now_ms, std::string *error = nullptr);
        bool fail(ClaimedInvocation &, std::string_view code, std::int64_t available_at_ms, std::string *error = nullptr);
        std::optional<ExecutionHandle> start(ClaimedInvocation &, ExecutionAdapter &,
            const nlohmann::json &, std::string *error = nullptr);
        ExecutionObservation observe(ClaimedInvocation &, ExecutionAdapter &,
            const ExecutionHandle &, std::string *error = nullptr);
        bool persist_observation(ClaimedInvocation&, ExecutionObservation&, IncrementalResultStore&,
            IncrementalStreamKind, std::string_view idempotency_key, std::string* error = nullptr);
        std::optional<ExecutionHandle> recover(ClaimedInvocation &, const ExecutionAdapterRegistry &,
            const nlohmann::json &, std::string *error = nullptr);

    private:
        StoreResult advance(LongRunningToolInvocation &, InvocationState, std::string, std::uint64_t, nlohmann::json = {});
        InvocationStore &store_;
        distributed::SQLiteDurableQueue &queue_;
        distributed::SQLiteWorkerRegistry &workers_;
        WorkerIdentity worker_;
        std::int64_t lease_ms_;
    };
}
