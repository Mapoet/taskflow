#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "agent/assurance/verifier.hpp"

namespace agent_framework::assurance {

class VerifierRegistry {
public:
    bool register_verifier(std::shared_ptr<Verifier> verifier);
    std::vector<std::shared_ptr<Verifier>> for_layer(VerificationLayer layer) const;
    std::vector<std::shared_ptr<Verifier>> all() const;
private:
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Verifier>> verifiers_;
};

}  // namespace agent_framework::assurance
