#include "agent/sandbox/provider.hpp"

#include <algorithm>
#include <filesystem>
#include <set>

namespace agent_framework::sandbox {

std::vector<SandboxValidationIssue> validate(const SandboxSpec& spec) {
    std::vector<SandboxValidationIssue> issues;
    auto add = [&](std::string code, std::string message) {
        issues.push_back({std::move(code), std::move(message)});
    };
    if(spec.metadata.identity.tenant_id.empty() || spec.metadata.identity.task_id.empty() ||
       spec.metadata.identity.run_id.empty()) add("identity_missing", "tenant/task/run identity is required");
    if(spec.provider.empty() || spec.command.empty()) add("execution_missing", "provider and command are required");
    if(spec.workspace_base_digest.empty() || spec.memory_view_digest.empty() || spec.policy_revision.empty())
        add("replay_binding_missing", "workspace, memory view, and policy revisions are required");
    if(spec.wall_time_ms == 0 || spec.memory_bytes == 0 || spec.cpu_millis == 0)
        add("resource_limit_missing", "CPU, memory, and wall limits must be non-zero");
    std::set<std::filesystem::path> ro;
    for(const auto& value : spec.read_only_mounts) {
        std::filesystem::path path(value);
        if(!path.is_absolute() || path.lexically_normal().string().find("..") != std::string::npos)
            add("mount_invalid", value);
        ro.insert(path.lexically_normal());
    }
    for(const auto& value : spec.writable_mounts) {
        std::filesystem::path path(value);
        if(!path.is_absolute() || path.lexically_normal().string().find("..") != std::string::npos)
            add("mount_invalid", value);
        if(ro.count(path.lexically_normal())) add("mount_conflict", value);
    }
    for(const auto& credential : spec.credential_refs)
        if(credential.find('=') != std::string::npos || credential.find("-----BEGIN") != std::string::npos)
            add("raw_credential_forbidden", "credential entries must be opaque references");
    for(const auto& endpoint : spec.network_allowlist)
        if(endpoint == "*" || endpoint == "0.0.0.0/0" || endpoint == "::/0")
            add("network_wildcard_forbidden", endpoint);
    return issues;
}

bool SandboxProviderRegistry::register_provider(std::shared_ptr<SandboxProvider> provider) {
    if(!provider || provider->id().empty()) return false;
    std::lock_guard lock(mutex_);
    return providers_.emplace(provider->id(), std::move(provider)).second;
}

std::shared_ptr<SandboxProvider> SandboxProviderRegistry::find(std::string_view id) const {
    std::lock_guard lock(mutex_);
    const auto found = providers_.find(std::string(id));
    return found == providers_.end() ? nullptr : found->second;
}

}  // namespace agent_framework::sandbox
