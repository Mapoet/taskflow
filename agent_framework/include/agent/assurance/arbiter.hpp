#pragma once

#include <string_view>

#include "agent/assurance/evidence.hpp"

namespace agent_framework::assurance {

struct ArbiterBindings {
    contracts::ContractMetadata metadata;
    std::string artifact_manifest_digest;
    std::string memory_snapshot_id;
    std::string verification_view_digest;
    std::string now;
};

struct ArbiterOptions {
    bool require_all_five_layers{true};
};

class AcceptanceArbiter {
public:
    AcceptanceArbiter(ArbiterOptions options = {}) : options_(options) {}
    AcceptanceReport decide(const AcceptanceContract& contract,
                            const EvidenceLedger& evidence,
                            const ArbiterBindings& bindings) const;
private:
    ArbiterOptions options_;
};

}  // namespace agent_framework::assurance
