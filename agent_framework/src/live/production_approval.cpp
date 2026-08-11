#include "agent/live/production_approval.hpp"

namespace agent_framework::live {
namespace {
void fail(std::string* error, std::string message) { if(error) *error = std::move(message); }
}

std::string production_approval_signing_digest(const RoleCertificationReport& pending,
                                               std::string_view approval_decision_id) {
    auto proposed = pending;
    proposed.state = RoleCertificationState::Certified;
    proposed.approval_decision_id = std::string(approval_decision_id);
    proposed.signature = {};
    return role_report_signing_digest(proposed);
}

StoreBackedProductionApprovalVerifier::StoreBackedProductionApprovalVerifier(
    approval::ApprovalStore& store, std::string tenant_id,
    std::string required_scope, std::string now)
    : store_(store), tenant_id_(std::move(tenant_id)),
      required_scope_(std::move(required_scope)), now_(std::move(now)) {}

bool StoreBackedProductionApprovalVerifier::verify(std::string_view report_signing_digest,
    std::string_view decision_id, std::string* error) {
    const auto request = store_.request(decision_id);
    const auto decision = store_.latest_decision(decision_id);
    if(!request || !decision) { fail(error, "approval request or decision missing"); return false; }
    if(request->metadata.identity.tenant_id != tenant_id_ ||
       decision->metadata.identity.tenant_id != tenant_id_) {
        fail(error, "approval tenant mismatch"); return false;
    }
    const auto request_digest = approval::encode(*request).at("canonical_digest").get<std::string>();
    if(decision->request_digest != request_digest || decision->approval_id != request->approval_id) {
        fail(error, "approval request digest mismatch"); return false;
    }
    if(decision->decision != approval::Decision::Approved) {
        fail(error, "approval is not approved"); return false;
    }
    if(request->request_kind != "production_live_certification" ||
       request->scope != required_scope_ || decision->scope != required_scope_) {
        fail(error, "approval scope mismatch"); return false;
    }
    if(request->requester_id.empty() || decision->reviewer_id.empty() ||
       request->requester_id == decision->reviewer_id) {
        fail(error, "approval separation of duties failed"); return false;
    }
    if(request->arguments_digest != report_signing_digest ||
       decision->arguments_digest != report_signing_digest) {
        fail(error, "approval is not bound to report digest"); return false;
    }
    if(request->policy_revision.empty() || decision->policy_revision != request->policy_revision) {
        fail(error, "approval policy revision mismatch"); return false;
    }
    if((!request->expires_at.empty() && request->expires_at < now_) ||
       (!decision->expires_at.empty() && decision->expires_at < now_)) {
        fail(error, "approval expired"); return false;
    }
    return true;
}

}  // namespace agent_framework::live
