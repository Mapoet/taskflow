#pragma once

#include <functional>
#include <string>

#include "agent/live/production_approval.hpp"
#include "agent/live/production_attestation.hpp"
#include "agent/live/production_bundle.hpp"

namespace agent_framework::live {

struct ProductionLiveRunnerOptions {
    std::string workflow_id;
    std::string now;
    std::string approval_decision_id;
    std::string approval_scope{"production:release"};
    std::function<SignatureEnvelope(std::string_view)> signer;
    std::function<bool(const SignatureEnvelope&)> signature_verifier;
    std::function<void(std::string_view, std::string_view)> alert;
    std::function<bool()> cancelled;
};

class ProductionLiveRunner {
public:
    ProductionLiveRunner(RoleCertificationStore& certification_store,
                         ProductionAttestationStore& attestation_store,
                         approval::ApprovalStore& approval_store,
                         LiveCellExecutor& executor);
    RoleCertificationResult run(const ProductionLiveBundle& bundle,
                                const ProductionLiveRunnerOptions& options);
private:
    RoleCertificationStore& certification_store_;
    ProductionAttestationStore& attestation_store_;
    approval::ApprovalStore& approval_store_;
    LiveCellExecutor& executor_;
};

}  // namespace agent_framework::live
