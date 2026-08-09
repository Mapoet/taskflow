#ifndef AGENT_MCP_LIFECYCLE_HPP
#define AGENT_MCP_LIFECYCLE_HPP

#include <agent/mcp_client/mcp_client.hpp>
#include <agent/skills/skill_supply_chain.hpp>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

namespace agent_framework {
class ToolBus;

enum class CapabilityLifecycleState { Discovered, Validated, Staged, Active, Draining, Removed, Failed };
struct CapabilityManifest {
    std::string id, version, origin, digest;
    std::vector<std::string> permissions;
    std::string kind = "mcp";
    std::optional<SkillSignatureEnvelope> signature;
};
struct CapabilityStatus { CapabilityManifest manifest; CapabilityLifecycleState state; std::size_t leases = 0; std::string failure; };
using CapabilityAuditSink = std::function<void(const CapabilityStatus&)>;

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
    explicit McpCapabilityRegistry(std::shared_ptr<ToolBus> toolbus, CapabilityAuditSink audit = {});
    void stage(CapabilityManifest manifest, std::shared_ptr<MCPClient> client);
    void set_trust_store(SkillTrustStore trust, bool require_signature = true);
    void save(const std::string& path) const;
    void load(const std::string& path);
    void activate(const std::string& id);
    void drain(const std::string& id);
    bool remove(const std::string& id);  // false while leased
    CapabilityLease acquire(const std::string& id);
    std::optional<CapabilityStatus> status(const std::string& id) const;
private:
    struct Entry { CapabilityStatus status; std::shared_ptr<MCPClient> client; };
    void notify(const CapabilityStatus& status) const noexcept;
    std::shared_ptr<ToolBus> toolbus_;
    CapabilityAuditSink audit_;
    std::optional<SkillTrustStore> trust_;
    bool require_signature_ = false;
    mutable std::mutex mutex_;
    std::map<std::string, Entry> entries_;
};
}
#endif
