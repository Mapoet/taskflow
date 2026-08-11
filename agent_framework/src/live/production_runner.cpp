#include "agent/live/production_runner.hpp"

namespace agent_framework::live {

ProductionLiveRunner::ProductionLiveRunner(RoleCertificationStore& certification_store,
    ProductionAttestationStore& attestation_store, approval::ApprovalStore& approval_store,
    LiveCellExecutor& executor)
    : certification_store_(certification_store), attestation_store_(attestation_store),
      approval_store_(approval_store), executor_(executor) {}

RoleCertificationResult ProductionLiveRunner::run(const ProductionLiveBundle& bundle,
    const ProductionLiveRunnerOptions& options) {
    RoleCertificationResult rejected;
    const auto errors = validate_production_live_bundle(bundle);
    if(!errors.empty()) {
        rejected.state = RoleCertificationState::Failed;
        rejected.error_code = "production_bundle_invalid";
        rejected.error_message = errors.front();
        return rejected;
    }
    if(bundle.evidence_level != LiveEvidenceLevel::ProductionCertified) {
        rejected.state = RoleCertificationState::Failed;
        rejected.error_code = "production_certified_level_required";
        rejected.error_message = "ProductionLiveRunner only signs production-certified bundles";
        return rejected;
    }
    StoreBackedCellEvidenceVerifier evidence_verifier(
        attestation_store_, role_live_matrix_digest(bundle.matrix));
    StoreBackedProductionApprovalVerifier approval_verifier(approval_store_,
        bundle.environment.metadata.identity.tenant_id, options.approval_scope, options.now);
    RoleCertificationOptions workflow_options;
    workflow_options.workflow_id = options.workflow_id;
    workflow_options.now = options.now;
    workflow_options.approval_decision_id = options.approval_decision_id;
    workflow_options.approval_validator = [&](std::string_view digest, std::string_view id) {
        return approval_verifier.verify(digest, id);
    };
    workflow_options.signer = options.signer;
    workflow_options.signature_verifier = [&](const SignatureEnvelope& envelope) {
        return envelope.algorithm == "ed25519" && options.signature_verifier &&
               options.signature_verifier(envelope);
    };
    workflow_options.cell_evidence_verifier = [&](const auto& environment, const auto& spec,
                                                   const auto& result, auto* error) {
        return evidence_verifier.verify(environment, spec, result, error);
    };
    workflow_options.alert = options.alert;
    workflow_options.cancelled = options.cancelled;
    RoleLiveCertificationWorkflow workflow(certification_store_, executor_);
    return workflow.run(bundle.environment, bundle.matrix, workflow_options);
}

}  // namespace agent_framework::live
