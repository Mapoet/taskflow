#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/approval/types.hpp"

namespace agent_framework::approval {

enum class ApprovalStoreStatus {
    Committed, Duplicate, NotFound, RevisionConflict, Invalid, Busy, Error
};

struct ApprovalStoreResult {
    ApprovalStoreStatus status{ApprovalStoreStatus::Error};
    std::uint64_t revision{0};
    std::string digest;
    std::string error;
    explicit operator bool() const noexcept { return status == ApprovalStoreStatus::Committed; }
};

class ApprovalStore {
public:
    virtual ~ApprovalStore() = default;
    virtual ApprovalStoreResult put_request(const ApprovalRequest& request) = 0;
    virtual std::optional<ApprovalRequest> request(std::string_view approval_id) = 0;
    virtual ApprovalStoreResult decide(const ApprovalDecision& decision,
                                       std::uint64_t expected_revision) = 0;
    virtual std::optional<ApprovalDecision> latest_decision(std::string_view approval_id) = 0;
    virtual std::vector<ApprovalDecision> decision_history(std::string_view approval_id) = 0;
    virtual std::vector<ApprovalRequest> pending(std::string_view tenant_id,
                                                 std::string_view now,
                                                 std::size_t limit) = 0;
};

struct SQLiteApprovalStoreOptions {
    int busy_timeout_ms{3000};
    bool require_private_permissions{true};
};

class SQLiteApprovalStore final : public ApprovalStore {
public:
    explicit SQLiteApprovalStore(std::string path, SQLiteApprovalStoreOptions options = {});
    ~SQLiteApprovalStore() override;
    SQLiteApprovalStore(const SQLiteApprovalStore&) = delete;
    SQLiteApprovalStore& operator=(const SQLiteApprovalStore&) = delete;

    ApprovalStoreResult put_request(const ApprovalRequest& request) override;
    std::optional<ApprovalRequest> request(std::string_view approval_id) override;
    ApprovalStoreResult decide(const ApprovalDecision& decision,
                               std::uint64_t expected_revision) override;
    std::optional<ApprovalDecision> latest_decision(std::string_view approval_id) override;
    std::vector<ApprovalDecision> decision_history(std::string_view approval_id) override;
    std::vector<ApprovalRequest> pending(std::string_view tenant_id, std::string_view now,
                                         std::size_t limit) override;

private:
    void migrate();
    void* db_{nullptr};
    std::string path_;
    SQLiteApprovalStoreOptions options_;
    std::mutex mutex_;
};

}  // namespace agent_framework::approval
