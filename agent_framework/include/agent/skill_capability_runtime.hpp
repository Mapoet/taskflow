#ifndef AGENT_SKILL_CAPABILITY_RUNTIME_HPP
#define AGENT_SKILL_CAPABILITY_RUNTIME_HPP

#include "mcp_client.hpp"
#include "skill_runtime.hpp"
#include "toolbus.hpp"

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace agent_framework {

inline constexpr const char* kSkillCapabilityConflict = "skill_capability_conflict";
inline constexpr const char* kSkillDescriptorInvalid = "skill_descriptor_invalid";
inline constexpr const char* kSkillPromptVariableMissing = "skill_prompt_variable_missing";
inline constexpr const char* kSkillPromptSourceDenied = "skill_prompt_source_denied";
inline constexpr const char* kSkillPromptSizeExceeded = "skill_prompt_size_exceeded";

std::string skill_capability_name(std::string_view skill_id,
                                  std::string_view capability_id);

struct SkillMcpToolSpec {
    std::string name;
    bool exported = false;
};

struct SkillMcpDescriptor {
    std::string server;
    std::string transport;
    std::string command;
    std::string url;
    std::vector<std::string> arguments;
    std::vector<std::string> tool_filters;
    std::vector<SkillMcpToolSpec> tools;
    std::map<std::string, std::string> secret_references;
    std::string startup = "eager";
};

SkillMcpDescriptor skill_parse_mcp_descriptor(const nlohmann::json& value);

using SkillMcpClientFactory = std::function<std::shared_ptr<MCPClient>(
    const SkillMcpDescriptor&, const SkillInvocationContext&)>;

struct SkillPromptSources {
    nlohmann::json input = nlohmann::json::object();
    nlohmann::json context = nlohmann::json::object();
    nlohmann::json task = nlohmann::json::object();
};

struct SkillPromptResult {
    bool ok = false;
    std::string text;
    nlohmann::json error = nlohmann::json::object();
};

struct SkillCapabilityBindOptions {
    /** Publish names into the shared ToolBus. Workflow runs keep this disabled. */
    bool publish_to_toolbus = true;
};

class SkillCapabilityBinding : public std::enable_shared_from_this<SkillCapabilityBinding> {
public:
    ~SkillCapabilityBinding();

    SkillCapabilityBinding(const SkillCapabilityBinding&) = delete;
    SkillCapabilityBinding& operator=(const SkillCapabilityBinding&) = delete;

    const std::string& skill_id() const noexcept { return entry_.id; }
    const std::vector<std::string>& registered_tools() const noexcept { return registered_tools_; }
    bool closed() const noexcept;
    void close(std::chrono::milliseconds timeout = std::chrono::milliseconds(1000)) noexcept;

    SkillPromptResult render_prompt(const std::string& resource_id,
                                    const SkillPromptSources& sources) const;
    nlohmann::json invoke_capability(
        const std::string& capability_id, const nlohmann::json& arguments,
        const ToolCallControl& control = {});
    std::optional<ToolSideEffect> capability_side_effect(
        const std::string& capability_id) const;

private:
    friend class SkillCapabilityRuntime;
    struct Capability;
    struct McpSession;

    SkillCapabilityBinding(SkillIndexEntry entry,
                           std::shared_ptr<const SkillManifest> manifest,
                           std::shared_ptr<SkillRuntime> runtime,
                           std::shared_ptr<ToolBus> toolbus,
                           SkillInvocationContext context,
                           SkillMcpClientFactory mcp_factory);

    nlohmann::json invoke(std::size_t index, const nlohmann::json& arguments,
                          const ToolCallControl& control);
    bool acquire_call() const;
    void release_call() const noexcept;

    SkillIndexEntry entry_;
    std::shared_ptr<const SkillManifest> manifest_;
    std::shared_ptr<SkillRuntime> runtime_;
    std::shared_ptr<ToolBus> toolbus_;
    SkillInvocationContext context_;
    SkillMcpClientFactory mcp_factory_;
    std::vector<std::shared_ptr<Capability>> capabilities_;
    std::vector<std::shared_ptr<McpSession>> mcp_sessions_;
    std::vector<std::string> registered_tools_;
    std::map<std::string, nlohmann::json> prompt_descriptors_;
    mutable std::mutex state_mutex_;
    mutable std::condition_variable state_cv_;
    mutable std::size_t active_calls_ = 0;
    bool closed_ = false;
    bool activated_ = false;
};

struct SkillCapabilityBindResult {
    std::shared_ptr<SkillCapabilityBinding> binding;
    nlohmann::json error = nlohmann::json::object();
    bool ok() const noexcept { return binding != nullptr; }
};

class SkillCapabilityRuntime {
public:
    SkillCapabilityRuntime(std::shared_ptr<SkillRegistry> registry,
                           std::shared_ptr<SkillLoader> loader,
                           std::shared_ptr<SkillRuntime> runtime,
                           std::shared_ptr<ToolBus> toolbus,
                           SkillMcpClientFactory mcp_factory = {});

    SkillCapabilityBindResult bind(const std::string& skill_id,
                                   SkillInvocationContext context,
                                   SkillCapabilityBindOptions options = {}) const;
    SkillCapabilityBindResult bind_snapshot(
        const SkillIndexEntry& entry, std::shared_ptr<const SkillManifest> manifest,
        SkillInvocationContext context, SkillCapabilityBindOptions options = {}) const;

private:
    std::shared_ptr<SkillRegistry> registry_;
    std::shared_ptr<SkillLoader> loader_;
    std::shared_ptr<SkillRuntime> runtime_;
    std::shared_ptr<ToolBus> toolbus_;
    SkillMcpClientFactory mcp_factory_;
};

} // namespace agent_framework

#endif
