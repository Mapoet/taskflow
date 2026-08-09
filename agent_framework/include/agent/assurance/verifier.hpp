#pragma once

#include <string>
#include <vector>

#include "agent/assurance/evidence.hpp"
#include "agent/memory_v2/view_engine.hpp"

namespace agent_framework::assurance {

struct VerificationContext {
    contracts::ContractMetadata metadata;
    AcceptanceContract contract;
    memory_v2::MemoryView verification_view;
    nlohmann::json artifact_manifest = nlohmann::json::object();
    std::string deadline;
};

struct VerifierResult {
    std::vector<VerificationEvidence> evidence;
    std::vector<Finding> findings;
    std::string error;
};

class Verifier {
public:
    virtual ~Verifier() = default;
    virtual std::string id() const = 0;
    virtual std::vector<VerificationLayer> layers() const = 0;
    virtual bool read_only() const noexcept { return true; }
    virtual VerifierResult verify(const VerificationContext& context,
                                  const std::vector<Criterion>& criteria) = 0;
};

}  // namespace agent_framework::assurance
