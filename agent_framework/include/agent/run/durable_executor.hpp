#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "agent/run/store.hpp"

namespace agent_framework::run {

enum class RecoveryDisposition { Continue, AwaitApproval, ReconcileEffect, Terminal, Invalid };

struct RecoveryDecision {
    RecoveryDisposition disposition{RecoveryDisposition::Invalid};
    std::optional<RunRecord> run;
    std::optional<EffectRecord> uncertain_effect;
    std::string error;
};

// Store-first integration seam for graph executors. It never executes an external effect during
// recovery: Prepared/Unknown effects must be reconciled by the owning adapter first.
class DurableRunCoordinator {
public:
    explicit DurableRunCoordinator(RunStore& store) : store_(store) {}
    StoreResult enter_node(RunCheckpoint checkpoint, std::uint64_t expected_revision,
                           std::string event_type, nlohmann::json event_payload,
                           std::optional<EffectRecord> effect = std::nullopt,
                           std::optional<Interruption> interruption = std::nullopt);
    RecoveryDecision recover(std::string_view run_id,
                             std::optional<std::string_view> active_effect_id = std::nullopt);
private:
    RunStore& store_;
};

}  // namespace agent_framework::run
