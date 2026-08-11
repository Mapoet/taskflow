#include <cassert>
#include <filesystem>

#include <unistd.h>

#include "agent/live/production_approval.hpp"

int main() {
    using namespace agent_framework;
    const auto path = std::filesystem::temp_directory_path() /
        ("phase4-production-approval-" + std::to_string(getpid()) + ".sqlite");
    std::filesystem::remove(path);
    approval::SQLiteApprovalStore store(path.string());
    const std::string report_digest = "sha256:report-signing-digest";
    approval::ApprovalRequest request;
    request.metadata.identity.tenant_id = "tenant-live";
    request.metadata.identity.task_id = "task-live";
    request.approval_id = "approval-live";
    request.request_kind = "production_live_certification";
    request.requester_id = "release-runner";
    request.scope = "production:release";
    request.reason = "certify live matrix";
    request.risk_level = "high";
    request.policy_revision = "live-policy-r1";
    request.arguments_digest = report_digest;
    request.created_at = "2026-08-11T00:00:00Z";
    request.expires_at = "2026-08-12T00:00:00Z";
    assert(store.put_request(request));
    approval::ApprovalDecision decision;
    decision.metadata = request.metadata;
    decision.approval_id = request.approval_id;
    decision.request_digest = approval::encode(request).at("canonical_digest");
    decision.reviewer_id = "accountable-reviewer";
    decision.decision = approval::Decision::Approved;
    decision.scope = request.scope;
    decision.reason = "evidence reviewed";
    decision.policy_revision = request.policy_revision;
    decision.arguments_digest = report_digest;
    decision.decided_at = "2026-08-11T01:00:00Z";
    decision.expires_at = request.expires_at;
    assert(store.decide(decision, 0));

    live::StoreBackedProductionApprovalVerifier verifier(
        store, "tenant-live", "production:release", "2026-08-11T02:00:00Z");
    std::string error;
    assert(verifier.verify(report_digest, "approval-live", &error));
    assert(!verifier.verify("sha256:tampered", "approval-live", &error));
    live::StoreBackedProductionApprovalVerifier wrong_tenant(
        store, "tenant-other", "production:release", "2026-08-11T02:00:00Z");
    assert(!wrong_tenant.verify(report_digest, "approval-live", &error));
    live::StoreBackedProductionApprovalVerifier expired(
        store, "tenant-live", "production:release", "2026-08-13T00:00:00Z");
    assert(!expired.verify(report_digest, "approval-live", &error));
    std::filesystem::remove(path);
    return 0;
}
