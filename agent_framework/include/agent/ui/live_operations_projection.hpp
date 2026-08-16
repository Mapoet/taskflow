#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "agent/toolbus/toolbus.hpp"
#include "agent/conversation/types.hpp"
#include "agent/tool_runtime/types.hpp"
#include "agent/tool_runtime/event_stream.hpp"
#include "agent/ui/store_backed_operations.hpp"
#include "agent/recovery/task_state_coordinator.hpp"

namespace agent_framework {

struct LiveOperationsIdentity {
    std::string tenant_id;
    std::string run_id;
    std::string task_id;
    std::string conversation_id;
    std::string turn_id;
};

/**
 * Revision-aware bridge from the live tool lifecycle to the canonical Operations projection.
 *
 * The bridge persists every accepted transition before publishing it.  It deliberately projects
 * display-safe metadata only: arguments, results, credentials and hidden reasoning never enter the
 * Operations snapshot.
 */
class LiveOperationsProjection {
public:
    using Publisher = std::function<void(const Phase4OperationsSnapshot&)>;

    LiveOperationsProjection(LiveOperationsIdentity identity,
                             std::shared_ptr<SQLiteOperationsSnapshotStore> store = {},
                             Publisher publisher = {});
    LiveOperationsProjection(Phase4OperationsSnapshot initial,
                             std::shared_ptr<SQLiteOperationsSnapshotStore> store = {},
                             Publisher publisher = {});

    void observe_tool(const ToolExecutionEvent& event);
    void observe_invocation(const tool_runtime::InvocationEvent& event);
    /** Project the unified conversation/harness event stream into live Operations. */
    void observe_runtime(const conversation::RuntimeEventEnvelope& event);
    /** Apply the authoritative correlated Task lifecycle decision. */
    void observe_task_coordination(
        const recovery::CorrelatedStateEvent& event,
        const recovery::TaskCoordinationDecision& decision);
    /** Drain replay/live events from the canonical subscription until timeout or terminal status. */
    std::size_t consume_invocations(tool_runtime::InvocationEventSubscription& subscription,
                                    std::chrono::milliseconds timeout,
                                    std::size_t limit = 256);
    Phase4OperationsSnapshot snapshot() const;

private:
    void initialize_locked();
    void publish_locked();

    mutable std::mutex mutex_;
    Phase4OperationsSnapshot snapshot_;
    std::shared_ptr<SQLiteOperationsSnapshotStore> store_;
    Publisher publisher_;
    std::uint64_t revision_{0};
};

}  // namespace agent_framework
