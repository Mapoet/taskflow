#pragma once

#include <set>

#include "agent/harness/runtime.hpp"
#include "agent/run/durable_executor.hpp"

namespace agent_framework::harness {

struct ProductionCompositionIssue {
    std::string code;
    HarnessStage stage{HarnessStage::Intake};
    std::string message;
};

struct ProductionCompositionReport {
    bool ready{false};
    std::string manifest_digest;
    std::vector<ProductionCompositionIssue> issues;
};

// Legacy callback wrapper retained for source compatibility. Production composition
// deliberately rejects it; use WorkflowHarnessStagePort for production wiring.
class ManifestHarnessStagePort final : public HarnessStagePort {
public:
    using Execute = CallbackHarnessStagePort::Execute;
    using Reconcile = CallbackHarnessStagePort::Reconcile;
    ManifestHarnessStagePort(std::string id, bool side_effecting,
                             std::string adapter_kind,
                             std::string adapter_manifest_digest,
                             Execute execute, Reconcile reconcile = {});
    std::string id() const override { return id_; }
    bool may_have_side_effects() const noexcept override { return side_effecting_; }
    bool production_ready() const noexcept override { return false; }
    std::string capability_manifest_digest() const override { return manifest_digest_; }
    HarnessStageResult execute(const HarnessStageRequest& request) override;
    std::optional<HarnessStageResult> reconcile(const HarnessStageRequest& request) override;
private:
    std::string id_;
    bool side_effecting_{false};
    std::string adapter_kind_;
    std::string manifest_digest_;
    Execute execute_;
    Reconcile reconcile_;
};

class Phase4ProductionComposition {
public:
    Phase4ProductionComposition(HarnessStore& harness_store, run::RunStore& run_store,
                                std::shared_ptr<HarnessCheckpointObserver> observer = {});
    bool bind(HarnessStage stage, std::shared_ptr<HarnessStagePort> port);
    ProductionCompositionReport validate() const;
    std::optional<Phase4HarnessRuntime> build(std::string* error = nullptr);
    run::DurableRunCoordinator& durable_runs() noexcept { return durable_runs_; }
private:
    HarnessStore& harness_store_;
    run::DurableRunCoordinator durable_runs_;
    HarnessPortRegistry ports_;
    std::shared_ptr<HarnessCheckpointObserver> observer_;
};

}  // namespace agent_framework::harness
