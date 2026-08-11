#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agent_framework::sandbox {

struct CredentialLease {
    std::string reference;
    std::string value;
    std::string expires_at;
};

class CredentialBroker {
public:
    using Resolver = std::function<std::optional<CredentialLease>(std::string_view)>;
    explicit CredentialBroker(Resolver resolver);
    std::optional<std::vector<CredentialLease>> resolve(
        const std::vector<std::string>& references, std::string* error = nullptr) const;
    static void redact(std::string& text, const std::vector<CredentialLease>& leases);
private:
    Resolver resolver_;
};
}  // namespace agent_framework::sandbox
