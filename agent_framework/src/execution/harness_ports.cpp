#include "agent/execution/harness_ports.hpp"

#include <nlohmann/json.hpp>

#include "agent/contracts/contract.hpp"

namespace agent_framework::execution {
namespace {
std::string json_digest(const nlohmann::json& value) {
    return contracts::canonical_digest(value).value_or("sha256:unavailable");
}
}

ArtifactExecutionHarnessPort::ArtifactExecutionHarnessPort(
    std::string id, WorkspaceArtifactExecutor& executor, ArtifactAction action,
    ArtifactJournal* journal, std::string parent_key)
    : id_(std::move(id)), executor_(executor), action_(std::move(action)),
      journal_(journal), parent_key_(std::move(parent_key)) {
    if(id_.empty()) throw std::invalid_argument("artifact execution port id is required");
}

harness::HarnessStageResult ArtifactExecutionHarnessPort::invoke(
    const harness::HarnessStageRequest& request) {
    auto action = action_;
    // Harness outbox identity is authoritative and prevents caller-controlled replay domains.
    action.idempotency_key = request.idempotency_key;
    std::optional<ArtifactJournalEntry> parent;
    if(journal_ && !parent_key_.empty()) parent = journal_->load(parent_key_);
    auto receipt = executor_.execute(request.checkpoint.metadata.identity.run_id, action,
        parent ? &parent->receipt.manifest : nullptr);
    harness::HarnessStageResult out;
    out.invocation_manifest_digest = request.request_digest;
    if(!receipt.succeeded) {
        out.outcome = harness::StageOutcome::Failed;
        out.error_code = receipt.error_code;
        out.error_message = receipt.error_message;
        return out;
    }
    out.outcome = harness::StageOutcome::Succeeded;
    out.output_digest = receipt.manifest.manifest_digest;
    out.effect_receipt_digest = receipt.effect_digest;
    out.pins.artifact_manifest_digest = receipt.manifest.manifest_digest;
    return out;
}

harness::HarnessStageResult ArtifactExecutionHarnessPort::execute(
    const harness::HarnessStageRequest& request) { return invoke(request); }
std::optional<harness::HarnessStageResult> ArtifactExecutionHarnessPort::reconcile(
    const harness::HarnessStageRequest& request) { return invoke(request); }

ArtifactAssuranceHarnessPort::ArtifactAssuranceHarnessPort(
    std::string id, ArtifactJournal& journal, FilesystemArtifactOracle& oracle,
    std::string key, std::vector<ArtifactRequirement> requirements)
    : id_(std::move(id)), journal_(journal), oracle_(oracle), key_(std::move(key)),
      requirements_(std::move(requirements)) {
    if(id_.empty() || key_.empty() || requirements_.empty())
        throw std::invalid_argument("artifact assurance port configuration is incomplete");
}

harness::HarnessStageResult ArtifactAssuranceHarnessPort::execute(
    const harness::HarnessStageRequest& request) {
    harness::HarnessStageResult out;
    out.invocation_manifest_digest = request.request_digest;
    auto entry = journal_.load(key_);
    if(!entry || !entry->receipt.succeeded) {
        out.outcome = harness::StageOutcome::ManualReview;
        out.error_code = "artifact_receipt_unavailable";
        return out;
    }
    const auto observations = oracle_.verify(entry->receipt.manifest, requirements_);
    nlohmann::json evidence = nlohmann::json::array();
    for(const auto& item : observations) {
        evidence.push_back({{"oracle_id", item.oracle_id}, {"requirement_id", item.requirement_id},
            {"passed", item.passed}, {"manifest_digest", item.artifact_manifest_digest},
            {"observed_digest", item.observed_digest}, {"finding_id", item.finding_id}});
        if(!item.passed) out.finding_ids.push_back(item.finding_id);
    }
    out.output_digest = json_digest(evidence);
    out.pins.acceptance_report_digest = out.output_digest;
    out.acceptance_decision = out.finding_ids.empty() ? "accepted" : "rejected";
    out.outcome = out.finding_ids.empty() ? harness::StageOutcome::Succeeded
                                          : harness::StageOutcome::NeedsRemediation;
    return out;
}
}  // namespace agent_framework::execution
