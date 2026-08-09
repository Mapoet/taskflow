#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "agent/assurance/types.hpp"

namespace agent_framework::assurance {

enum class OracleStrength {
    Deterministic,
    RealSystem,
    Authoritative,
    StaticAnalysis,
    CalibratedModel,
    UncalibratedClaim
};

struct VerificationEvidence {
    std::string evidence_id;
    std::string criterion_id;
    std::string source_kind;
    std::string source_locator;
    std::string content_digest;
    std::string observed_at;
    std::string freshness_deadline;
    OracleStrength oracle_strength{OracleStrength::UncalibratedClaim};
    FindingOutcome outcome{FindingOutcome::Inconclusive};
    bool independent{true};
};

class EvidenceLedger {
public:
    bool append(VerificationEvidence evidence, std::string* error = nullptr);
    std::vector<VerificationEvidence> for_criterion(std::string_view criterion_id) const;
    std::vector<VerificationEvidence> all() const;
private:
    mutable std::mutex mutex_;
    std::map<std::string, VerificationEvidence> evidence_;
};

}  // namespace agent_framework::assurance
