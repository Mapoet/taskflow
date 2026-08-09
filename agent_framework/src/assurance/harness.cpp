#include "agent/assurance/harness.hpp"

#include <algorithm>
#include <map>
#include <utility>

namespace agent_framework::assurance {

AssuranceHarness::AssuranceHarness(
    memory_v2::MemoryViewEngine& views, VerifierRegistry& verifiers,
    AcceptanceArbiter arbiter)
    : views_(views), verifiers_(verifiers), arbiter_(std::move(arbiter)) {}

HarnessResult AssuranceHarness::verify(
    const AcceptanceContract& contract, const memory_v2::MemoryScope& subject,
    const nlohmann::json& artifacts, std::string_view now, std::string_view deadline) {
    HarnessResult result;
    auto spec = memory_v2::make_view_spec(memory_v2::MemoryViewMode::Verification,
                                           contract.metadata, subject);
    auto view = views_.build(spec, now);
    if(view.fail_closed) {
        result.fail_closed = true;
        result.verifier_errors.push_back("verification_view:" + view.error);
        result.report.decision = AcceptanceDecision::ManualReview;
        return result;
    }
    EvidenceLedger ledger;
    VerificationContext context{contract.metadata, contract, view, artifacts,
                                std::string(deadline)};
    for(const auto& verifier : verifiers_.all()) {
        std::vector<Criterion> assigned;
        const auto layers = verifier->layers();
        for(const auto& criterion : contract.criteria)
            if(std::find(layers.begin(), layers.end(), criterion.layer) != layers.end())
                assigned.push_back(criterion);
        if(assigned.empty()) continue;
        auto verifier_result = verifier->verify(context, assigned);
        if(!verifier_result.error.empty()) {
            result.verifier_errors.push_back(verifier->id() + ":" + verifier_result.error);
            continue;
        }
        for(auto& evidence : verifier_result.evidence) {
            std::string error;
            if(!ledger.append(std::move(evidence), &error))
                result.verifier_errors.push_back(verifier->id() + ":evidence:" + error);
        }
    }
    ArbiterBindings bindings;
    bindings.metadata = contract.metadata;
    bindings.artifact_manifest_digest = artifacts.empty() ? "" :
        contracts::embedded_digest(artifacts).value_or("");
    bindings.memory_snapshot_id = view.snapshot.snapshot_id;
    bindings.verification_view_digest = view.manifest.view_digest;
    bindings.now = std::string(now);
    result.report = arbiter_.decide(contract, ledger, bindings);
    if(!result.verifier_errors.empty() && result.report.decision == AcceptanceDecision::Accepted)
        result.report.decision = AcceptanceDecision::ManualReview;
    return result;
}

}  // namespace agent_framework::assurance
