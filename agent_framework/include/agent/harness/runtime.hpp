#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "agent/harness/store.hpp"
#include "agent/ui/phase4_operations.hpp"

namespace agent_framework::harness {

struct HarnessStageRequest {
    HarnessCheckpoint checkpoint;
    HarnessStage stage{HarnessStage::Intake};
    std::uint64_t attempt{0};
    std::string effect_id;
    std::string idempotency_key;
    std::string request_digest;
    std::function<bool()> cancelled;
};

struct HarnessStageResult {
    StageOutcome outcome{StageOutcome::Failed};
    PinnedRevisions pins;
    std::string invocation_manifest_digest;
    std::string output_digest;
    std::string effect_receipt_digest;
    std::vector<std::string> finding_ids;
    std::string acceptance_decision;
    std::string error_code;
    std::string error_message;
};

class HarnessStagePort {
public:
    virtual ~HarnessStagePort() = default;
    virtual std::string id() const = 0;
    virtual bool may_have_side_effects() const noexcept = 0;
    virtual bool production_ready() const noexcept { return false; }
    virtual std::string capability_manifest_digest() const { return {}; }
    virtual HarnessStageResult execute(const HarnessStageRequest& request) = 0;
    virtual std::optional<HarnessStageResult> reconcile(
        const HarnessStageRequest&) { return std::nullopt; }
};

class CallbackHarnessStagePort final : public HarnessStagePort {
public:
    using Execute = std::function<HarnessStageResult(const HarnessStageRequest&)>;
    using Reconcile = std::function<std::optional<HarnessStageResult>(
        const HarnessStageRequest&)>;

    CallbackHarnessStagePort(std::string port_id, bool side_effecting,
                             Execute execute, Reconcile reconcile = {});
    std::string id() const override { return id_; }
    bool may_have_side_effects() const noexcept override { return side_effecting_; }
    HarnessStageResult execute(const HarnessStageRequest& request) override;
    std::optional<HarnessStageResult> reconcile(
        const HarnessStageRequest& request) override;

private:
    std::string id_;
    bool side_effecting_{false};
    Execute execute_;
    Reconcile reconcile_;
};

class HarnessPortRegistry {
public:
    bool bind(HarnessStage stage, std::shared_ptr<HarnessStagePort> port);
    std::shared_ptr<HarnessStagePort> find(HarnessStage stage) const;
    std::vector<HarnessStage> bound_stages() const;

private:
    std::map<HarnessStage, std::shared_ptr<HarnessStagePort>> ports_;
};

struct HarnessStart {
    contracts::ContractMetadata metadata;
    std::string harness_id;
    std::string intake_digest;
    std::string acceptance_contract_digest;
    std::string profile_revision_digest;
    std::string prompt_revision_digest;
    std::uint64_t max_remediation_cycles{2};
    bool judge_required{true};
};

struct HarnessRuntimeOptions {
    std::uint64_t max_stage_attempts{2};
    std::uint64_t max_transitions_per_run{64};
    std::function<bool()> cancelled;
    std::function<std::string()> now;
    std::function<void(HarnessStage)> after_stage_effect;
};

struct HarnessRunResult {
    HarnessState state{HarnessState::Failed};
    HarnessCheckpoint checkpoint;
    std::string error_code;
    std::string error_message;
};

class Phase4HarnessRuntime {
public:
    Phase4HarnessRuntime(HarnessStore& store, HarnessPortRegistry ports);

    HarnessRunResult run(const HarnessStart& start,
                         const HarnessRuntimeOptions& options = {});
    HarnessRunResult resume(std::string_view tenant_id, std::string_view harness_id,
                            const HarnessRuntimeOptions& options = {});

    static std::vector<std::string> completion_gate_issues(
        const HarnessCheckpoint& checkpoint);
    static Phase4OperationsSnapshot project_operations(
        const HarnessCheckpoint& checkpoint);

private:
    HarnessRunResult drive(HarnessCheckpoint checkpoint,
                           const HarnessRuntimeOptions& options);

    HarnessStore& store_;
    HarnessPortRegistry ports_;
};

}  // namespace agent_framework::harness
