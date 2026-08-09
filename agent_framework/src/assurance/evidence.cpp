#include "agent/assurance/evidence.hpp"

#include <algorithm>

namespace agent_framework::assurance {

bool EvidenceLedger::append(VerificationEvidence evidence, std::string* error) {
    if(evidence.evidence_id.empty() || evidence.criterion_id.empty() ||
       evidence.source_locator.empty() || evidence.content_digest.empty()) {
        if(error) *error = "evidence id, criterion, locator, and digest are required";
        return false;
    }
    std::lock_guard lock(mutex_);
    if(evidence_.count(evidence.evidence_id)) {
        if(error) *error = "evidence id exists";
        return false;
    }
    const auto duplicate = std::find_if(evidence_.begin(), evidence_.end(), [&](const auto& item) {
        return item.second.criterion_id == evidence.criterion_id &&
               item.second.content_digest == evidence.content_digest;
    });
    if(duplicate != evidence_.end()) {
        if(error) *error = "duplicate evidence content";
        return false;
    }
    evidence_.emplace(evidence.evidence_id, std::move(evidence));
    return true;
}

std::vector<VerificationEvidence> EvidenceLedger::for_criterion(std::string_view criterion) const {
    std::lock_guard lock(mutex_);
    std::vector<VerificationEvidence> result;
    for(const auto& [id, evidence] : evidence_) {
        (void)id;
        if(evidence.criterion_id == criterion) result.push_back(evidence);
    }
    return result;
}

std::vector<VerificationEvidence> EvidenceLedger::all() const {
    std::lock_guard lock(mutex_);
    std::vector<VerificationEvidence> result;
    for(const auto& [id, evidence] : evidence_) { (void)id; result.push_back(evidence); }
    return result;
}

}  // namespace agent_framework::assurance
