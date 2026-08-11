#pragma once

#include "agent/execution/artifact_executor.hpp"
#include "agent/harness/runtime.hpp"

namespace agent_framework::execution {

class ArtifactExecutionHarnessPort final : public harness::HarnessStagePort {
public:
    ArtifactExecutionHarnessPort(std::string port_id, WorkspaceArtifactExecutor& executor,
                                 ArtifactAction action, ArtifactJournal* journal = nullptr,
                                 std::string parent_idempotency_key = {});
    std::string id() const override { return id_; }
    bool may_have_side_effects() const noexcept override { return true; }
    harness::HarnessStageResult execute(const harness::HarnessStageRequest& request) override;
    std::optional<harness::HarnessStageResult> reconcile(
        const harness::HarnessStageRequest& request) override;
private:
    harness::HarnessStageResult invoke(const harness::HarnessStageRequest& request);
    std::string id_;
    WorkspaceArtifactExecutor& executor_;
    ArtifactAction action_;
    ArtifactJournal* journal_{nullptr};
    std::string parent_key_;
};

class ArtifactAssuranceHarnessPort final : public harness::HarnessStagePort {
public:
    ArtifactAssuranceHarnessPort(std::string port_id, ArtifactJournal& journal,
                                 FilesystemArtifactOracle& oracle,
                                 std::string execution_idempotency_key,
                                 std::vector<ArtifactRequirement> requirements);
    std::string id() const override { return id_; }
    bool may_have_side_effects() const noexcept override { return false; }
    harness::HarnessStageResult execute(const harness::HarnessStageRequest& request) override;
private:
    std::string id_;
    ArtifactJournal& journal_;
    FilesystemArtifactOracle& oracle_;
    std::string key_;
    std::vector<ArtifactRequirement> requirements_;
};

}  // namespace agent_framework::execution
