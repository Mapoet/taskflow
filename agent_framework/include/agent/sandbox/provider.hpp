#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "agent/sandbox/types.hpp"

namespace agent_framework::sandbox {

struct SandboxValidationIssue { std::string code; std::string message; };
std::vector<SandboxValidationIssue> validate(const SandboxSpec& spec);

struct SandboxHandle { std::string sandbox_id; std::string spec_digest; };
struct ExecResult {
    int exit_code{-1};
    std::string stdout_text;
    std::string stderr_text;
    bool timed_out{false};
    SandboxManifest manifest;
};
enum class SandboxSignal { Cooperative, Terminate, Kill };
struct SandboxCancelResult { bool accepted{false},terminal{false},effect_known{false};std::string diagnostic,receipt_digest; };

class SandboxProvider {
public:
    virtual ~SandboxProvider() = default;
    virtual std::string id() const = 0;
    virtual std::string version() const = 0;
    virtual bool available(std::string* reason = nullptr) const = 0;
    virtual std::optional<SandboxHandle> create(const SandboxSpec& spec,
                                                std::string* error = nullptr) = 0;
    virtual std::optional<ExecResult> exec(const SandboxHandle& handle,
                                           std::string* error = nullptr) = 0;
    virtual bool destroy(const SandboxHandle& handle, std::string* error = nullptr) = 0;
    virtual SandboxCancelResult cancel(const SandboxHandle&,SandboxSignal){return {false,false,false,"sandbox cancellation unsupported",{}};}
};

class SandboxProviderRegistry {
public:
    bool register_provider(std::shared_ptr<SandboxProvider> provider);
    std::shared_ptr<SandboxProvider> find(std::string_view provider_id) const;
private:
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<SandboxProvider>> providers_;
};

}  // namespace agent_framework::sandbox
