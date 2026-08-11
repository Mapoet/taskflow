#pragma once

#include <filesystem>
#include <map>
#include <mutex>

#include "agent/sandbox/credential_broker.hpp"
#include "agent/sandbox/provider.hpp"

namespace agent_framework::sandbox {

struct ProcessSandboxOptions {
    std::filesystem::path unshare_path{"/usr/bin/unshare"};
    std::filesystem::path bubblewrap_path{"/usr/bin/bwrap"};
    std::uint64_t output_limit_bytes{1024U * 1024U};
    std::uint64_t workspace_quota_bytes{256U * 1024U * 1024U};
};

class BubblewrapSandboxProvider final : public SandboxProvider {
public:
    BubblewrapSandboxProvider(ProcessSandboxOptions options,
                              std::shared_ptr<CredentialBroker> credentials = {});
    std::string id() const override { return "bubblewrap"; }
    std::string version() const override { return "bubblewrap-v1"; }
    bool available(std::string* reason = nullptr) const override;
    std::optional<SandboxHandle> create(const SandboxSpec& spec,
                                        std::string* error = nullptr) override;
    std::optional<ExecResult> exec(const SandboxHandle& handle,
                                   std::string* error = nullptr) override;
    bool destroy(const SandboxHandle& handle, std::string* error = nullptr) override;
private:
    ProcessSandboxOptions options_;
    std::shared_ptr<CredentialBroker> credentials_;
    std::mutex mutex_;
    std::map<std::string, SandboxSpec> specs_;
};
}  // namespace agent_framework::sandbox
