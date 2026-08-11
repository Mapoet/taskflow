#include "agent/run/durable_executor.hpp"

namespace agent_framework::run {

StoreResult DurableRunCoordinator::enter_node(
    RunCheckpoint checkpoint, std::uint64_t expected_revision, std::string event_type,
    nlohmann::json event_payload, std::optional<EffectRecord> effect,
    std::optional<Interruption> interruption) {
    return store_.commit({std::move(checkpoint), expected_revision, std::move(event_type),
                          std::move(event_payload), std::move(effect),
                          std::move(interruption)});
}

RecoveryDecision DurableRunCoordinator::recover(
    std::string_view run_id, std::optional<std::string_view> active_effect_id) {
    auto run = store_.load(run_id);
    if(!run) return {RecoveryDisposition::Invalid, std::nullopt, std::nullopt, "run not found"};
    if(active_effect_id) {
        auto effect = store_.effect(run_id, *active_effect_id);
        if(effect && (effect->state == EffectState::Prepared || effect->state == EffectState::Unknown))
            return {RecoveryDisposition::ReconcileEffect, run, effect, {}};
    }
    switch(run->checkpoint.state) {
    case RunState::AwaitingApproval:
    case RunState::Interrupted:
        return {RecoveryDisposition::AwaitApproval, run, std::nullopt, {}};
    case RunState::Completed:
    case RunState::Partial:
    case RunState::Rejected:
    case RunState::Failed:
    case RunState::Cancelled:
        return {RecoveryDisposition::Terminal, run, std::nullopt, {}};
    default:
        return {RecoveryDisposition::Continue, run, std::nullopt, {}};
    }
}

}  // namespace agent_framework::run
