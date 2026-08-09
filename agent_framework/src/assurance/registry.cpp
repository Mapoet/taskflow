#include "agent/assurance/registry.hpp"

#include <algorithm>

namespace agent_framework::assurance {

bool VerifierRegistry::register_verifier(std::shared_ptr<Verifier> verifier) {
    if(!verifier || verifier->id().empty() || !verifier->read_only()) return false;
    std::lock_guard lock(mutex_);
    return verifiers_.emplace(verifier->id(), std::move(verifier)).second;
}

std::vector<std::shared_ptr<Verifier>> VerifierRegistry::for_layer(VerificationLayer layer) const {
    std::vector<std::shared_ptr<Verifier>> result;
    for(const auto& verifier : all()) {
        const auto layers = verifier->layers();
        if(std::find(layers.begin(), layers.end(), layer) != layers.end()) result.push_back(verifier);
    }
    return result;
}

std::vector<std::shared_ptr<Verifier>> VerifierRegistry::all() const {
    std::lock_guard lock(mutex_);
    std::vector<std::shared_ptr<Verifier>> result;
    for(const auto& [id, verifier] : verifiers_) { (void)id; result.push_back(verifier); }
    return result;
}

}  // namespace agent_framework::assurance
