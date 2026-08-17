#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "agent/session/run_supervisor.hpp"

namespace agent_framework::session {

enum class WorkerDisposition { Completed, AwaitingInput, Failed, Cancelled };
struct WorkerExecutionResult {
    WorkerDisposition disposition{WorkerDisposition::Completed};
    std::string diagnostic;
};
struct WorkerExecutionContext {
    SupervisedRun run;
    std::vector<SessionRunCommand> commands;
    // A production executor calls this around bounded work units. Failure means
    // the lease/fencing token is stale and execution must stop immediately.
    std::function<bool(std::uint64_t now_ms)> heartbeat;
};
struct WorkerTickResult {
    bool claimed{false},executed{false};
    std::string run_id,error;
    SupervisedRunState state{SupervisedRunState::Queued};
    std::uint64_t revision{0},lease_epoch{0};
};

class SessionRunWorker {
public:
    using Executor=std::function<WorkerExecutionResult(WorkerExecutionContext&)>;
    SessionRunWorker(SQLiteSessionRunSupervisor&,std::string worker_id,
                     std::uint64_t lease_ms,Executor);
    WorkerTickResult tick(std::uint64_t now_ms);
private:
    SQLiteSessionRunSupervisor& supervisor_;std::string worker_id_;
    std::uint64_t lease_ms_;Executor executor_;
};

}  // namespace agent_framework::session
