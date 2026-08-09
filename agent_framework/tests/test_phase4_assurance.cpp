#include <cassert>
#include <memory>

#include "agent/assurance/harness.hpp"

namespace {
using namespace agent_framework;

class ClaimsProvider final : public memory_v2::MemoryProvider {
public:
    std::string id() const override { return "claims"; }
    memory_v2::ProviderResult fetch(const memory_v2::MemoryQuery& query) override {
        memory_v2::MemoryRecord record;
        record.metadata.identity.tenant_id = query.subject.tenant_id;
        record.metadata.identity.task_id = query.subject.task_id;
        record.record_id = "executor-says-done";
        record.scope = query.subject;
        record.scope.level = memory_v2::MemoryLevel::Task;
        record.kind = memory_v2::MemoryKind::Evidentiary;
        record.status = memory_v2::MemoryStatus::Candidate;
        record.authority = memory_v2::Authority::Candidate;
        record.source_kind = "executor_claim";
        record.trust_class = "unverified_executor";
        record.content_type = "application/json";
        record.content = {{"claim", "everything passed"}};
        return {id(), 1, {record}, {}};
    }
};

class FiveLayerVerifier final : public assurance::Verifier {
public:
    bool metric_fails{true};
    std::string id() const override { return "five-layer"; }
    std::vector<assurance::VerificationLayer> layers() const override {
        using L = assurance::VerificationLayer;
        return {L::Functional, L::Module, L::Integration, L::System, L::Metric};
    }
    assurance::VerifierResult verify(
        const assurance::VerificationContext& context,
        const std::vector<assurance::Criterion>& criteria) override {
        assert(context.verification_view.records.empty());
        assurance::VerifierResult result;
        for(const auto& criterion : criteria) {
            assurance::VerificationEvidence evidence;
            evidence.evidence_id = "evidence:" + criterion.criterion_id;
            evidence.criterion_id = criterion.criterion_id;
            evidence.source_kind = "test";
            evidence.source_locator = "test://" + criterion.criterion_id;
            evidence.content_digest = "sha256:" + criterion.criterion_id;
            evidence.observed_at = "2026-08-09T00:00:00Z";
            evidence.freshness_deadline = "2027-01-01T00:00:00Z";
            evidence.oracle_strength = assurance::OracleStrength::Deterministic;
            evidence.outcome = metric_fails &&
                criterion.layer == assurance::VerificationLayer::Metric
                ? assurance::FindingOutcome::Fail : assurance::FindingOutcome::Pass;
            result.evidence.push_back(std::move(evidence));
        }
        return result;
    }
};
}

int main() {
    using namespace agent_framework;
    memory_v2::MemoryProviderRegistry providers;
    assert(providers.register_provider(std::make_shared<ClaimsProvider>()));
    memory_v2::MemoryViewEngine views(providers);
    assurance::VerifierRegistry verifiers;
    auto verifier = std::make_shared<FiveLayerVerifier>();
    assert(verifiers.register_verifier(verifier));
    assurance::AssuranceHarness harness(views, verifiers);

    assurance::AcceptanceContract contract;
    contract.metadata.identity.tenant_id = "tenant-a";
    contract.metadata.identity.principal_id = "verifier-a";
    contract.metadata.identity.project_id = "project-a";
    contract.metadata.identity.task_id = "task-a";
    contract.plan_digest = "sha256:plan";
    using L = assurance::VerificationLayer;
    contract.criteria = {
        {"functional", L::Functional, "user goal works", "runtime", {"test"}, "pass", true},
        {"module", L::Module, "module invariant", "test", {"test"}, "pass", true},
        {"integration", L::Integration, "upstream integration", "runtime", {"test"}, "pass", true},
        {"system", L::System, "artifact complete", "artifact", {"test"}, "pass", true},
        {"metric", L::Metric, "quality threshold", "metric", {"test"}, ">=0.95", true}};
    memory_v2::MemoryScope subject;
    subject.tenant_id = "tenant-a";
    subject.project_id = "project-a";
    subject.task_id = "task-a";

    const auto missing_artifact = harness.verify(contract, subject, nlohmann::json::object(),
                                                  "2026-08-09T00:00:00Z");
    assert(missing_artifact.report.decision == assurance::AcceptanceDecision::Rejected);
    const auto metric_failure = harness.verify(contract, subject, {{"artifact", "sha256:artifact"}},
                                                "2026-08-09T00:00:00Z");
    assert(metric_failure.report.decision == assurance::AcceptanceDecision::Rejected);
    verifier->metric_fails = false;
    const auto accepted = harness.verify(contract, subject, {{"artifact", "sha256:artifact"}},
                                         "2026-08-09T00:00:00Z");
    assert(accepted.report.decision == assurance::AcceptanceDecision::Accepted);
    assert(accepted.report.findings.size() == 5);

    assurance::EvidenceLedger conflict;
    assurance::VerificationEvidence pass{"pass", "metric", "model", "model://claim",
        "sha256:model", "", "", assurance::OracleStrength::CalibratedModel,
        assurance::FindingOutcome::Pass, true};
    assurance::VerificationEvidence fail{"fail", "metric", "test", "test://metric",
        "sha256:test", "", "", assurance::OracleStrength::Deterministic,
        assurance::FindingOutcome::Fail, true};
    assert(conflict.append(pass));
    assert(conflict.append(fail));
    assurance::AcceptanceContract metric_only = contract;
    metric_only.criteria = {contract.criteria.back()};
    assurance::ArbiterBindings bindings{contract.metadata, "sha256:artifact", "snapshot", "view", ""};
    auto report = assurance::AcceptanceArbiter({false}).decide(metric_only, conflict, bindings);
    assert(report.decision == assurance::AcceptanceDecision::Rejected);
    return 0;
}
