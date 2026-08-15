#pragma once

#include <functional>
#include <string>

#include "agent/conversation/types.hpp"
#include "agent/conversation/conversation_engine.hpp"

namespace agent_framework::conversation {

enum class TurnExecutionPath { Harness, LegacyReactFallback, FailClosed };

struct HarnessSupportedRuntimePolicy {
    bool production{false};
    bool harness_ready{false};
    bool explicit_legacy_fallback{false};
};

struct TurnExecutionDecision {
    TurnExecutionPath path{TurnExecutionPath::FailClosed};
    std::string reason_code;
};

struct HarnessSupportedTurnRequest {
    TurnRequest turn;
    TurnCheckpoint checkpoint;
};

class HarnessSupportedTurnRuntime {
public:
    using Executor = std::function<ModelTurnOutcome(const HarnessSupportedTurnRequest&)>;

    HarnessSupportedTurnRuntime(HarnessSupportedRuntimePolicy policy,
                                Executor harness_executor,
                                Executor legacy_fallback = {},
                                RuntimeEventSink events = {});

    static TurnExecutionDecision route(const HarnessSupportedRuntimePolicy& policy,
                                       TaskExecutionProfile profile);
    ModelTurnOutcome execute(const TurnRequest& request,
                             const TurnCheckpoint& checkpoint) const;

private:
    void emit(const TurnRequest& request, TurnExecutionPath path,
              std::string_view reason) const;

    HarnessSupportedRuntimePolicy policy_;
    Executor harness_executor_;
    Executor legacy_fallback_;
    RuntimeEventSink events_;
};

const char* turn_execution_path_name(TurnExecutionPath path) noexcept;

}  // namespace agent_framework::conversation
