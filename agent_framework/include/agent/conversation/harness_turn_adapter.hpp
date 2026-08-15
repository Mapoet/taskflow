#pragma once

#include <functional>

#include "agent/conversation/harness_supported_runtime.hpp"
#include "agent/harness/runtime.hpp"

namespace agent_framework::conversation {

/**
 * Non-production harness composition for interactive deployments.
 *
 * Control always enters Phase4HarnessRuntime.  The supplied model/tool executor is confined to the
 * Execution stage and cannot decide closure.  Production deployments must use
 * DefaultProductionCompositionBuilder instead of this adapter.
 */
class HarnessTurnAdapter {
public:
    using Execution = std::function<ModelTurnOutcome(const HarnessSupportedTurnRequest&)>;
    using ProjectionSink = std::function<void(const Phase4OperationsSnapshot&)>;
    using TraceSink = std::function<void(const harness::HarnessEvent&)>;

    HarnessTurnAdapter(harness::HarnessStore& store, Execution execution,
                       ProjectionSink projection = {}, TraceSink trace = {});
    ModelTurnOutcome execute(const HarnessSupportedTurnRequest& request);

private:
    harness::HarnessStore& store_;
    Execution execution_;
    ProjectionSink projection_;
    TraceSink trace_;
};

}  // namespace agent_framework::conversation
