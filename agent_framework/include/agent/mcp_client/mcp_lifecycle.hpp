#ifndef AGENT_MCP_LIFECYCLE_HPP
#define AGENT_MCP_LIFECYCLE_HPP

#include <agent/mcp_client/mcp_client.hpp>
#include <agent/skills/skill_supply_chain.hpp>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace agent_framework {
class ToolBus;

enum class CapabilityLifecycleState { Discovered, Validated, Staged, Active, Draining, Removed, Failed };

struct CapabilityTransportDescriptor {
    std::string kind;              // "http" or "stdio"
    std::string endpoint;          // HTTPS URL for HTTP
    std::string command;           // absolute executable path for stdio
    std::vector<std::string> arguments;
    std::string executable_digest;
    std::vector<std::string> environment_allowlist;
    std::string credential_ref;
    std::string working_directory;
};

struct CapabilityManifest {
    std::string id, version, origin, digest;
    std::vector<std::string> permissions;
    std::string kind = "mcp";
    std::optional<SkillSignatureEnvelope> signature;
    CapabilityTransportDescriptor transport;
    std::vector<std::string> tenant_visibility{"default"};
    std::vector<std::string> dependency_lock;
};
struct CapabilityStatus {
    CapabilityManifest manifest;
    CapabilityLifecycleState state;
    std::size_t leases = 0;
    std::string failure;
    std::uint64_t revision = 0;
    CapabilityLifecycleState desired_state = CapabilityLifecycleState::Staged;
};
using CapabilityAuditSink = std::function<void(const CapabilityStatus&)>;
using McpClientFactory = std::function<std::shared_ptr<MCPClient>(const CapabilityManifest&)>;

json canonical_capability_manifest(const CapabilityManifest& manifest);
std::string capability_manifest_digest(const CapabilityManifest& manifest);

class CapabilityLease {
public:
    CapabilityLease() = default;
    CapabilityLease(CapabilityLease&& other) noexcept;
    CapabilityLease& operator=(CapabilityLease&& other) noexcept;
    ~CapabilityLease();
    CapabilityLease(const CapabilityLease&) = delete;
    CapabilityLease& operator=(const CapabilityLease&) = delete;
    explicit operator bool() const noexcept { return static_cast<bool>(release_); }
private:
    friend class McpCapabilityRegistry;
    explicit CapabilityLease(std::function<void()> release) : release_(std::move(release)) {}
    std::function<void()> release_;
};

class McpCapabilityRegistry {
public:
    explicit McpCapabilityRegistry(std::shared_ptr<ToolBus> toolbus,
                                   CapabilityAuditSink audit = {},
                                   bool require_signature = true);
    ~McpCapabilityRegistry();
    void stage(CapabilityManifest manifest, std::shared_ptr<MCPClient> client);
    void set_trust_store(SkillTrustStore trust, bool require_signature = true);
    void set_development_unsigned_allowed(bool allowed);
    void save(const std::string& path) const;
    void load(const std::string& path);
    void rehydrate(const std::string& id, McpClientFactory factory);
    void rebind(const std::string& id, std::shared_ptr<MCPClient> client);
    void activate(const std::string& id);
    void drain(const std::string& id);
    bool remove(const std::string& id);  // false while leased
    CapabilityLease acquire(const std::string& id,
                            std::uint64_t expected_revision = 0,
                            std::string expected_digest = {});
    std::optional<CapabilityStatus> status(const std::string& id) const;
private:
    struct RegistryState;
    void validate_manifest(const CapabilityManifest& manifest) const;
    void notify(const CapabilityStatus& status) const noexcept;
    std::shared_ptr<ToolBus> toolbus_;
    CapabilityAuditSink audit_;
    std::optional<SkillTrustStore> trust_;
    bool require_signature_ = true;
    bool development_unsigned_allowed_ = false;
    std::shared_ptr<RegistryState> state_;
};
}
#endif
