#include "agent/harness/production_composition.hpp"

#include <array>
#include <stdexcept>

#include "agent/contracts/contract.hpp"
#include "agent/harness/workflow_adapter.hpp"

namespace agent_framework::harness {
namespace {
constexpr std::array<HarnessStage, 11> kRequiredStages = {
    HarnessStage::Intake, HarnessStage::Cognition, HarnessStage::PlanApproval,
    HarnessStage::Execution, HarnessStage::MemoryUpdate, HarnessStage::Assurance,
    HarnessStage::Remediation, HarnessStage::Reexecution, HarnessStage::Reverification,
    HarnessStage::Judge, HarnessStage::Operations};
}

ManifestHarnessStagePort::ManifestHarnessStagePort(
    std::string id, bool side_effecting, std::string adapter_kind,
    std::string adapter_manifest_digest, Execute execute, Reconcile reconcile)
    : id_(std::move(id)), side_effecting_(side_effecting),
      adapter_kind_(std::move(adapter_kind)), manifest_digest_(std::move(adapter_manifest_digest)),
      execute_(std::move(execute)), reconcile_(std::move(reconcile)) {
    if(id_.empty() || adapter_kind_.empty() || manifest_digest_.empty() || !execute_)
        throw std::invalid_argument("production stage adapter requires identity, kind, manifest and execute");
    if(side_effecting_ && !reconcile_)
        throw std::invalid_argument("side-effecting production adapter requires reconciliation");
}
HarnessStageResult ManifestHarnessStagePort::execute(const HarnessStageRequest& request) {
    return execute_(request);
}
std::optional<HarnessStageResult> ManifestHarnessStagePort::reconcile(
    const HarnessStageRequest& request) {
    return reconcile_ ? reconcile_(request) : std::nullopt;
}

Phase4ProductionComposition::Phase4ProductionComposition(
    HarnessStore& harness_store, run::RunStore& run_store)
    : harness_store_(harness_store), durable_runs_(run_store) {}

bool Phase4ProductionComposition::bind(
    HarnessStage stage, std::shared_ptr<HarnessStagePort> port) {
    return ports_.bind(stage, std::move(port));
}

ProductionCompositionReport Phase4ProductionComposition::validate() const {
    ProductionCompositionReport report;
    nlohmann::json manifest = nlohmann::json::array();
    for(const auto stage : kRequiredStages) {
        const auto port = ports_.find(stage);
        if(!port) {
            report.issues.push_back({"required_port_missing", stage,
                "production stage port is missing: " + harness_stage_name(stage)});
            continue;
        }
        const auto digest = port->capability_manifest_digest();
        if(!port->production_ready() || digest.empty() ||
           dynamic_cast<WorkflowHarnessStagePort*>(port.get()) == nullptr) {
            report.issues.push_back({"port_not_production_ready", stage,
                "stage port must be a typed workflow adapter with observability and a durable capability manifest"});
            continue;
        }
        manifest.push_back({{"stage", harness_stage_name(stage)}, {"port", port->id()},
                            {"side_effecting", port->may_have_side_effects()},
                            {"capability_manifest_digest", digest}});
    }
    report.ready = report.issues.empty();
    if(report.ready) report.manifest_digest = contracts::canonical_digest(manifest).value_or("");
    return report;
}

std::optional<Phase4HarnessRuntime> Phase4ProductionComposition::build(std::string* error) {
    const auto report = validate();
    if(!report.ready) {
        if(error) *error = report.issues.empty() ? "production composition invalid"
            : report.issues.front().code + ":" + report.issues.front().message;
        return std::nullopt;
    }
    return Phase4HarnessRuntime(harness_store_, std::move(ports_));
}

}  // namespace agent_framework::harness
