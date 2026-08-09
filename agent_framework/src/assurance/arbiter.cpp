#include "agent/assurance/arbiter.hpp"

#include <algorithm>
#include <array>
#include <set>

namespace agent_framework::assurance {
namespace {
int strength(OracleStrength value) { return static_cast<int>(value); }
}

AcceptanceReport AcceptanceArbiter::decide(
    const AcceptanceContract& contract, const EvidenceLedger& ledger,
    const ArbiterBindings& bindings) const {
    AcceptanceReport report;
    report.metadata = bindings.metadata;
    report.plan_digest = contract.plan_digest;
    report.acceptance_contract_digest =
        encode(contract).at("canonical_digest").get<std::string>();
    report.artifact_manifest_digest = bindings.artifact_manifest_digest;
    report.memory_snapshot_id = bindings.memory_snapshot_id;
    report.verification_view_digest = bindings.verification_view_digest;
    bool mandatory_failed = false;
    bool mandatory_inconclusive = false;
    bool optional_failed = false;
    std::set<VerificationLayer> layers;
    for(const auto& criterion : contract.criteria) {
        layers.insert(criterion.layer);
        auto candidates = ledger.for_criterion(criterion.criterion_id);
        Finding finding;
        finding.finding_id = "arbiter:" + criterion.criterion_id;
        finding.criterion_id = criterion.criterion_id;
        finding.severity = criterion.mandatory ? "mandatory" : "optional";
        int best_pass = 99;
        int best_fail = 99;
        bool stale = false;
        std::set<std::string> kinds;
        for(const auto& evidence : candidates) {
            if(!evidence.independent) continue;
            if(!evidence.freshness_deadline.empty() && !bindings.now.empty() &&
               evidence.freshness_deadline < bindings.now) { stale = true; continue; }
            finding.evidence_ids.push_back(evidence.evidence_id);
            kinds.insert(evidence.source_kind);
            if(evidence.outcome == FindingOutcome::Pass)
                best_pass = std::min(best_pass, strength(evidence.oracle_strength));
            else if(evidence.outcome == FindingOutcome::Fail)
                best_fail = std::min(best_fail, strength(evidence.oracle_strength));
        }
        bool requirements_met = true;
        for(const auto& required : criterion.required_evidence)
            if(!kinds.count(required)) requirements_met = false;
        if(!requirements_met || candidates.empty() || (best_pass == 99 && best_fail == 99)) {
            finding.outcome = FindingOutcome::Inconclusive;
            finding.remediation = stale ? "refresh stale evidence" : "collect required independent evidence";
        } else if(best_fail <= best_pass) {
            finding.outcome = FindingOutcome::Fail;
            finding.remediation = "remediate the highest-strength counter-evidence and reverify";
        } else {
            finding.outcome = FindingOutcome::Pass;
            finding.confidence = 1.0 - 0.1 * best_pass;
        }
        if(finding.outcome == FindingOutcome::Fail) {
            if(criterion.mandatory) mandatory_failed = true; else optional_failed = true;
        } else if(finding.outcome != FindingOutcome::Pass && criterion.mandatory) {
            mandatory_inconclusive = true;
        }
        report.findings.push_back(std::move(finding));
    }
    if(options_.require_all_five_layers && layers.size() != 5) {
        mandatory_inconclusive = true;
        report.residual_risks.push_back("acceptance contract does not cover all five verification layers");
    }
    if(bindings.artifact_manifest_digest.empty()) {
        mandatory_inconclusive = true;
        report.residual_risks.push_back("artifact manifest is missing");
    }
    if(mandatory_failed) report.decision = AcceptanceDecision::Rejected;
    else if(mandatory_inconclusive) report.decision = AcceptanceDecision::ManualReview;
    else if(optional_failed) report.decision = AcceptanceDecision::Partial;
    else report.decision = AcceptanceDecision::Accepted;
    return report;
}

}  // namespace agent_framework::assurance
