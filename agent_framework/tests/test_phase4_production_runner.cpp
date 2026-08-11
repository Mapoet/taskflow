#include <cassert>
#include <filesystem>

#include <unistd.h>

#include "agent/live/production_runner.hpp"
#include "phase4_live_test_support.hpp"

namespace {
using namespace agent_framework;
using namespace agent_framework::live;

class AttestingExecutor final : public LiveCellExecutor {
public:
    AttestingExecutor(ProductionAttestationStore& store) : store_(store) {}
    LiveCellResult execute(const LiveCellExecutionRequest& request) override {
        phase4_live_test::DeterministicExecutor delegate;
        auto result = delegate.execute(request);
        ProductionCellAttestation value;
        value.tenant_id = request.environment.metadata.identity.tenant_id;
        value.attestation_id = "audit-" + result.invocation_id;
        value.environment_digest = role_environment_digest(request.environment);
        value.matrix_digest = role_live_matrix_digest(request.matrix);
        value.cell_id = request.cell.cell_id;
        value.spec_digest = result.spec_digest;
        value.invocation_id = result.invocation_id;
        value.invocation_manifest_digest = result.invocation_manifest_digest;
        value.result_digest = production_cell_result_digest(result);
        value.evidence_digests = result.evidence_digests;
        value.oracle_digests = result.oracle_digests;
        value.trace_id = "trace-production-runner";
        value.source = "independent-audit-bridge";
        value.source_signature_digest = "sha256:audit-signature";
        value.recorded_at = "2026-08-11T01:00:01Z";
        std::string error;
        assert(store_.append(value, &error));
        return result;
    }
private:
    ProductionAttestationStore& store_;
};
}

int main() {
    using namespace agent_framework;
    using namespace agent_framework::live;
    using namespace phase4_live_test;
    const auto root = std::filesystem::temp_directory_path() /
        ("phase4-production-runner-" + std::to_string(getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    auto environment_value = environment("task-r6l-runner");
    auto matrix_value = matrix(environment_value, false);
    ProductionLiveBundle bundle;
    bundle.evidence_level = LiveEvidenceLevel::ProductionCertified;
    bundle.environment = environment_value;
    bundle.matrix = matrix_value;
    bundle.mandatory_dependency_digests = environment_value.dependency_digests;
    for(const auto& cell : matrix_value.cells) bundle.mandatory_cell_ids.push_back(cell.cell_id);

    SQLiteRoleCertificationStore certifications((root / "certifications.sqlite").string());
    SQLiteProductionAttestationStore attestations((root / "attestations.sqlite").string());
    approval::SQLiteApprovalStore approvals((root / "approvals.sqlite").string());
    AttestingExecutor executor(attestations);
    ProductionLiveRunner runner(certifications, attestations, approvals, executor);
    ProductionLiveRunnerOptions options;
    options.workflow_id = "production-runner";
    options.now = "2026-08-11T02:00:00Z";
    const auto pending = runner.run(bundle, options);
    assert(pending.state == RoleCertificationState::AwaitingApproval && pending.report);

    approval::ApprovalRequest request;
    request.metadata = environment_value.metadata;
    request.approval_id = "approval-production";
    request.request_kind = "production_live_certification";
    request.requester_id = "scheduled-runner";
    request.scope = options.approval_scope;
    request.reason = "review complete live evidence";
    request.risk_level = "high";
    request.policy_revision = "live-policy-r1";
    request.arguments_digest = production_approval_signing_digest(*pending.report, request.approval_id);
    request.created_at = options.now;
    request.expires_at = "2026-08-12T00:00:00Z";
    assert(approvals.put_request(request));
    approval::ApprovalDecision decision;
    decision.metadata = request.metadata;
    decision.approval_id = request.approval_id;
    decision.request_digest = approval::encode(request).at("canonical_digest");
    decision.reviewer_id = "release-owner";
    decision.decision = approval::Decision::Approved;
    decision.scope = request.scope;
    decision.reason = "attestations reviewed";
    decision.policy_revision = request.policy_revision;
    decision.arguments_digest = request.arguments_digest;
    decision.decided_at = "2026-08-11T02:10:00Z";
    decision.expires_at = request.expires_at;
    assert(approvals.decide(decision, 0));

    options.approval_decision_id = request.approval_id;
    options.signer = [](std::string_view digest) {
        return SignatureEnvelope{"ed25519", "kms://production/key", std::string(digest),
                                 "verified:" + std::string(digest)};
    };
    options.signature_verifier = [](const SignatureEnvelope& envelope) {
        return envelope.signature == "verified:" + envelope.signed_digest;
    };
    const auto certified = runner.run(bundle, options);
    assert(certified.state == RoleCertificationState::Certified && certified.report);
    assert(certified.report->executed && certified.report->blockers.empty());
    std::filesystem::remove_all(root);
    return 0;
}
