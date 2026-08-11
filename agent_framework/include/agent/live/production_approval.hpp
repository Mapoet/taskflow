#pragma once

#include <string>
#include <string_view>

#include "agent/approval/store.hpp"
#include "agent/live/role_certification.hpp"

namespace agent_framework::live {

std::string production_approval_signing_digest(const RoleCertificationReport& pending,
                                               std::string_view approval_decision_id);

class StoreBackedProductionApprovalVerifier {
public:
    StoreBackedProductionApprovalVerifier(approval::ApprovalStore& store,
                                          std::string tenant_id,
                                          std::string required_scope,
                                          std::string now);
    bool verify(std::string_view report_signing_digest,
                std::string_view decision_id,
                std::string* error = nullptr);
private:
    approval::ApprovalStore& store_;
    std::string tenant_id_;
    std::string required_scope_;
    std::string now_;
};

}  // namespace agent_framework::live
