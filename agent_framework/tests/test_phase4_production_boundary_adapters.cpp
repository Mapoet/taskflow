#include <cassert>
#include <filesystem>

#include "agent/harness/production_boundary_adapters.hpp"
#include "agent/internal/platform_io.hpp"

namespace {
using namespace agent_framework;
class Repository final : public harness::ProductionWorkflowInputRepository {
public:
    planning::TaskIntake value;
    std::optional<planning::TaskIntake> intake(const contracts::ContractIdentity&) override { return value; }
    std::optional<memory_v2::workflows::MemoryWorkflowInput> memory_input(const contracts::ContractIdentity&,std::string_view) override{return std::nullopt;}
    std::optional<assurance::AcceptanceContract> acceptance_contract(const contracts::ContractIdentity&,std::string_view) override{return std::nullopt;}
    std::optional<nlohmann::json> task_context(const contracts::ContractIdentity&,std::string_view) override{return std::nullopt;}
    std::optional<nlohmann::json> artifact_manifest(const contracts::ContractIdentity&,std::string_view) override{return std::nullopt;}
    std::optional<assurance::AcceptanceReport> acceptance_report(const contracts::ContractIdentity&,std::string_view) override{return std::nullopt;}
    std::optional<assurance::AssuranceCheckpoint> assurance_checkpoint(const contracts::ContractIdentity&,std::string_view) override{return std::nullopt;}
    std::optional<remediation::ImpactInventory> impact_inventory(const contracts::ContractIdentity&,std::string_view) override{return std::nullopt;}
    std::optional<harness::JudgeWorkflowInput> evaluation_input(const contracts::ContractIdentity&) override{return std::nullopt;}
};
}

int main() {
    using namespace agent_framework;
    namespace fs = std::filesystem;
    const auto root=fs::temp_directory_path()/("phase4-boundary-"+std::to_string(internal::current_process_id()));
    std::error_code ec;fs::remove_all(root,ec);fs::create_directories(root);
    Repository repository;
    repository.value.metadata.identity.tenant_id="tenant";
    repository.value.metadata.identity.task_id="task";
    repository.value.metadata.identity.run_id="run";
    repository.value.user_goal="deliver";
    repository.value.requested_deliverables={"artifact"};
    repository.value.success_signals={"verified"};
    harness::HarnessStageRequest request;
    request.checkpoint.metadata=repository.value.metadata;
    request.checkpoint.harness_id="harness";
    request.checkpoint.pins.intake_digest=planning::encode(repository.value).at("canonical_digest");
    request.request_digest="sha256:request";
    harness::StoreBackedIntakeAdapter intake(repository,"r1","sha256:config");
    assert(intake.run(request).result.outcome==harness::StageOutcome::Succeeded);
    request.checkpoint.pins.intake_digest="sha256:tampered";
    assert(intake.run(request).result.error_code=="intake_revision_mismatch");

    approval::SQLiteApprovalStore approvals((root/"approval.sqlite3").string());
    request.checkpoint.pins.plan_digest="sha256:plan";
    approval::ApprovalRequest approval_request;
    approval_request.metadata=request.checkpoint.metadata;
    approval_request.approval_id="harness:plan-approval";
    approval_request.request_kind="plan";approval_request.requester_id="author";
    approval_request.scope="plan";approval_request.reason="risk";approval_request.risk_level="high";
    approval_request.policy_revision="p1";approval_request.plan_digest="sha256:plan";
    approval_request.created_at="2026-08-12T00:00:00Z";approval_request.expires_at="2026-08-13T00:00:00Z";
    assert(approvals.put_request(approval_request));
    harness::StoreBackedApprovalAdapter approval_adapter(approvals,[]{return "2026-08-12T01:00:00Z";},"r1","sha256:config");
    assert(approval_adapter.run(request).result.outcome==harness::StageOutcome::AwaitingApproval);
    approval::ApprovalDecision decision;decision.metadata=approval_request.metadata;decision.approval_id=approval_request.approval_id;
    decision.request_digest=approval::encode(approval_request).at("canonical_digest");decision.reviewer_id="reviewer";
    decision.decision=approval::Decision::Approved;decision.scope="plan";decision.reason="reviewed";
    decision.policy_revision="p1";decision.plan_digest="sha256:plan";decision.decided_at="2026-08-12T00:30:00Z";decision.expires_at=approval_request.expires_at;
    assert(approvals.decide(decision,0));
    const auto approved=approval_adapter.run(request).result;
    assert(approved.outcome==harness::StageOutcome::Succeeded);
    assert(approved.pins.approval_decision_id=="harness:plan-approval");

    execution::SQLiteArtifactJournal journal((root/"artifact.sqlite3").string());
    execution::WorkspaceArtifactExecutor executor(root/"workspace",&journal);
    harness::ArtifactExecutionWorkflowAdapter execution_adapter(executor,
        {"write",execution::ArtifactActionKind::WriteText,"output.txt","ok","",true},
        journal,"r1","sha256:config");
    request.checkpoint.metadata.identity.run_id="run";request.stage=harness::HarnessStage::Execution;
    request.idempotency_key="effect-1";
    const auto executed=execution_adapter.run(request).result;
    assert(executed.outcome==harness::StageOutcome::Succeeded);
    assert(!executed.effect_receipt_digest.empty()&&!executed.pins.artifact_manifest_digest.empty());
    assert(execution_adapter.reconcile(request)->result.output_digest==executed.output_digest);
    fs::remove_all(root,ec);
}
