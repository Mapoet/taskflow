#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "agent/planning/types.hpp"

namespace agent_framework::planning {

enum class PlanningCommitStatus { Committed, Duplicate, NotFound, RevisionConflict, Invalid, Busy, Error };
struct PlanningCommitResult {
    PlanningCommitStatus status{PlanningCommitStatus::Invalid};
    std::string digest;
    std::string error;
    explicit operator bool() const noexcept { return status == PlanningCommitStatus::Committed; }
};

class EvidenceStore {
public:
    virtual ~EvidenceStore() = default;
    virtual PlanningCommitResult append(const contracts::ContractMetadata& scope,
                                        const EvidenceRecord& record) = 0;
    virtual std::optional<EvidenceRecord> get(const contracts::ContractMetadata& scope,
                                              std::string_view evidence_id) = 0;
    virtual EvidenceBundle bundle(const contracts::ContractMetadata& scope,
                                  const std::vector<std::string>& evidence_ids) = 0;
};

class InMemoryEvidenceStore final : public EvidenceStore {
public:
    PlanningCommitResult append(const contracts::ContractMetadata& scope,
                                const EvidenceRecord& record) override;
    std::optional<EvidenceRecord> get(const contracts::ContractMetadata& scope,
                                      std::string_view evidence_id) override;
    EvidenceBundle bundle(const contracts::ContractMetadata& scope,
                          const std::vector<std::string>& evidence_ids) override;

private:
    static std::string scope_key(const contracts::ContractMetadata& scope);
    std::mutex mutex_;
    std::map<std::string, EvidenceRecord> records_;
};

}  // namespace agent_framework::planning
