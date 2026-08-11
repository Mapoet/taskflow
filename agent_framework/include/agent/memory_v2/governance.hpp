#pragma once

#include <memory>
#include <string>
#include <vector>

#include "agent/memory_v2/store.hpp"
#include "agent/approval/store.hpp"

namespace agent_framework::memory_v2 {

struct GovernanceResult {
    CommitResult commit;
    std::vector<std::string> completed_sinks;
    std::vector<std::string> inconclusive_sinks;
};

class ForgetSink {
public:
    virtual ~ForgetSink() = default;
    virtual std::string id() const = 0;
    virtual bool erase(std::string_view record_id, std::string* error) = 0;
};

class MemoryGovernanceService {
public:
    explicit MemoryGovernanceService(std::shared_ptr<MemoryStore> store);
    bool register_forget_sink(std::shared_ptr<ForgetSink> sink);
    CommitResult promote(std::string_view record_id, std::uint64_t expected_revision,
                         MemoryStatus target_status, Authority target_authority,
                         std::string_view decision_id, std::string_view approval_id = {});
    GovernanceResult forget(std::string_view record_id, std::uint64_t expected_revision,
                            std::string_view approval_id);

private:
    std::shared_ptr<MemoryStore> store_;
    std::vector<std::shared_ptr<ForgetSink>> sinks_;
};

class ApprovalBoundMemoryGovernance {
public:
    ApprovalBoundMemoryGovernance(std::shared_ptr<MemoryStore> store,
                                  approval::ApprovalStore& approvals);
    bool register_forget_sink(std::shared_ptr<ForgetSink> sink);
    CommitResult promote(std::string_view record_id, std::uint64_t expected_revision,
                         MemoryStatus target_status, Authority target_authority,
                         std::string_view evidence_decision_id,
                         std::string_view approval_id, std::string_view now);
    CommitResult correct(const MemoryRecord& corrected, std::uint64_t expected_revision,
                         const std::vector<std::string>& evidence_ids,
                         std::string_view approval_id, std::string_view now);
    GovernanceResult forget(std::string_view record_id, std::uint64_t expected_revision,
                            std::string_view approval_id, std::string_view now);
private:
    std::string validate(std::string_view record_id, std::uint64_t revision,
                         std::string_view approval_id, std::string_view now);
    std::shared_ptr<MemoryStore> store_;
    approval::ApprovalStore& approvals_;
    MemoryGovernanceService governance_;
};

}  // namespace agent_framework::memory_v2
