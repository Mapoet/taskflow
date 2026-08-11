#include "agent/sandbox/credential_broker.hpp"

#include <set>
#include <stdexcept>

namespace agent_framework::sandbox {
CredentialBroker::CredentialBroker(Resolver resolver) : resolver_(std::move(resolver)) {
    if (!resolver_) throw std::invalid_argument("credential resolver is required");
}
std::optional<std::vector<CredentialLease>> CredentialBroker::resolve(
    const std::vector<std::string>& references, std::string* error) const {
    std::set<std::string> seen; std::vector<CredentialLease> result;
    for (const auto& reference : references) {
        if (reference.empty() || reference.find('=') != std::string::npos ||
            reference.find('\n') != std::string::npos || !seen.insert(reference).second) {
            if (error) *error = "invalid or duplicate credential reference";
            return std::nullopt;
        }
        auto lease = resolver_(reference);
        if (!lease || lease->reference != reference || lease->value.empty() ||
            lease->value.size() > 65536U) {
            if (error) *error = "credential reference unavailable";
            return std::nullopt;
        }
        result.push_back(std::move(*lease));
    }
    return result;
}
void CredentialBroker::redact(std::string& text, const std::vector<CredentialLease>& leases) {
    for (const auto& lease : leases) {
        if (lease.value.empty()) continue;
        std::size_t at = 0;
        while ((at = text.find(lease.value, at)) != std::string::npos) {
            text.replace(at, lease.value.size(), "[REDACTED]"); at += 10;
        }
    }
}
}  // namespace agent_framework::sandbox
