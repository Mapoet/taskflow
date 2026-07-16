#include <agent/skill_capability_runtime.hpp>

#include <agent/schema_validate.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <stdexcept>

namespace agent_framework {
namespace {

using json = nlohmann::json;

json failure(const char* code, const std::string& message,
             json details = json::object()) {
    return {{"error", message}, {"code", code}, {"details", std::move(details)}};
}

bool wildcard_match(std::string_view pattern, std::string_view value) {
    std::size_t p = 0;
    std::size_t v = 0;
    std::size_t star = std::string_view::npos;
    std::size_t retry = 0;
    while (v < value.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == value[v])) {
            ++p;
            ++v;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            retry = v;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            v = ++retry;
        } else return false;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool selected(const std::vector<std::string>& filters, const std::string& name) {
    if (filters.empty()) return true;
    bool has_allow = false;
    bool allowed = false;
    for (const auto& filter : filters) {
        const bool deny = !filter.empty() && filter.front() == '!';
        const std::string_view pattern = deny ? std::string_view(filter).substr(1) : filter;
        if (!deny) has_allow = true;
        if (!wildcard_match(pattern, name)) continue;
        if (deny) return false;
        allowed = true;
    }
    return !has_allow || allowed;
}

SkillResourceKind loader_kind(SkillResourceType kind) {
    if (kind == SkillResourceType::Tool) return SkillResourceKind::Tool;
    if (kind == SkillResourceType::Mcp) return SkillResourceKind::Mcp;
    if (kind == SkillResourceType::Prompt) return SkillResourceKind::Prompt;
    return SkillResourceKind::Template;
}

json load_descriptor(const SkillIndexEntry& entry, const SkillResourceDescriptor& resource,
                     const std::shared_ptr<SkillLoader>& loader) {
    std::string error;
    auto content = loader->load_resource(entry.id, resource.path, loader_kind(resource.kind),
                                         resource.size_limit.value_or(1024U * 1024U), &error);
    if (!content) throw std::runtime_error("resource load failed: " + error);
    try {
        auto value = json::parse(*content);
        if (!value.is_object()) throw std::runtime_error("descriptor must be a JSON object");
        return value;
    } catch (const json::exception& error) {
        throw std::runtime_error(std::string("descriptor JSON is invalid: ") + error.what());
    }
}

std::vector<std::string> string_array(const json& object, const char* key) {
    std::vector<std::string> out;
    if (!object.contains(key)) return out;
    if (!object.at(key).is_array()) throw std::runtime_error(std::string(key) + " must be an array");
    for (const auto& value : object.at(key)) {
        if (!value.is_string()) throw std::runtime_error(std::string(key) + " must contain strings");
        out.push_back(value.get<std::string>());
    }
    return out;
}

} // namespace

SkillMcpDescriptor skill_parse_mcp_descriptor(const nlohmann::json& value) {
    SkillMcpDescriptor out;
    out.server = value.value("server", std::string{});
    out.transport = value.value("transport", std::string{});
    out.command = value.value("command", std::string{});
    out.url = value.value("url", std::string{});
    out.startup = value.value("startup", std::string("eager"));
    out.arguments = string_array(value, "args");
    out.tool_filters = string_array(value, "tool-filters");
    if (out.server.empty()) throw std::runtime_error("server is required");
    if (out.transport != "stdio" && out.transport != "http" && out.transport != "mock")
        throw std::runtime_error("transport must be stdio, http, or mock");
    if (out.startup != "eager" && out.startup != "lazy")
        throw std::runtime_error("startup must be eager or lazy");
    if (out.transport == "stdio" && out.command.empty())
        throw std::runtime_error("stdio command is required");
    if (out.transport == "http" && out.url.empty())
        throw std::runtime_error("http url is required");
    if (value.contains("secret-references")) {
        if (!value.at("secret-references").is_object())
            throw std::runtime_error("secret-references must be an object");
        for (auto it = value.at("secret-references").begin();
             it != value.at("secret-references").end(); ++it) {
            if (!it.value().is_string())
                throw std::runtime_error("secret-references values must be strings");
            out.secret_references[it.key()] = it.value().get<std::string>();
        }
    }
    if (value.contains("tools")) {
        if (!value.at("tools").is_array()) throw std::runtime_error("tools must be an array");
        for (const auto& tool : value.at("tools")) {
            if (tool.is_string()) out.tools.push_back({tool.get<std::string>(), false});
            else if (tool.is_object() && tool.contains("name") && tool.at("name").is_string())
                out.tools.push_back({tool.at("name").get<std::string>(),
                                     tool.value("export", false)});
            else throw std::runtime_error("each MCP tool must have a name");
        }
    }
    if (out.startup == "lazy" && out.tools.empty())
        throw std::runtime_error("lazy MCP startup requires an explicit tools list");
    return out;
}

namespace {

std::shared_ptr<MCPClient> default_mcp_factory(const SkillMcpDescriptor& descriptor,
                                               const SkillInvocationContext& context) {
    std::map<std::string, std::string> injected;
    for (const auto& [target, reference] : descriptor.secret_references) {
        if (!context.secret_provider) throw std::runtime_error("secret provider is unavailable");
        auto value = context.secret_provider(reference);
        if (!value) throw std::runtime_error("secret reference is unavailable: " + reference);
        injected[target] = std::move(*value);
    }
    if (descriptor.transport == "stdio")
        return MCPClient::create_stdio(descriptor.command, descriptor.arguments, injected);
    if (descriptor.transport == "http")
        return MCPClient::create_http(descriptor.url, injected);
    throw std::runtime_error("mock MCP transport requires an injected factory");
}

std::string pointer_value(const json& source, const std::string& pointer, bool& found) {
    found = false;
    try {
        const auto& value = pointer.empty() ? source : source.at(json::json_pointer(pointer));
        found = true;
        return value.is_string() ? value.get<std::string>() : value.dump();
    } catch (...) {
        return {};
    }
}

void replace_all(std::string& text, const std::string& needle, const std::string& value) {
    std::size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string::npos) {
        text.replace(offset, needle.size(), value);
        offset += value.size();
    }
}

void validate_prompt_descriptor(const json& descriptor) {
    if (!descriptor.contains("template") || !descriptor.at("template").is_string())
        throw std::runtime_error("template must be a string");
    if (descriptor.contains("max-bytes") && !descriptor.at("max-bytes").is_number_unsigned())
        throw std::runtime_error("max-bytes must be an unsigned integer");
    if (!descriptor.contains("variables")) return;
    if (!descriptor.at("variables").is_object())
        throw std::runtime_error("variables must be an object");
    for (auto it = descriptor.at("variables").begin();
         it != descriptor.at("variables").end(); ++it) {
        if (!it.value().is_object())
            throw std::runtime_error("variable descriptor must be an object: " + it.key());
        if (!it.value().contains("source") || !it.value().at("source").is_string())
            throw std::runtime_error("variable source must be a string: " + it.key());
        if (it.value().contains("path") && !it.value().at("path").is_string())
            throw std::runtime_error("variable path must be a string: " + it.key());
        if (it.value().contains("required") && !it.value().at("required").is_boolean())
            throw std::runtime_error("variable required must be a boolean: " + it.key());
        if (it.value().contains("schema") && !it.value().at("schema").is_object())
            throw std::runtime_error("variable schema must be an object: " + it.key());
    }
}

} // namespace

struct SkillCapabilityBinding::McpSession {
    SkillMcpDescriptor descriptor;
    std::shared_ptr<MCPClient> client;
    mutable std::mutex mutex;
};

struct SkillCapabilityBinding::Capability {
    enum class Kind { Local, Mcp };
    Kind kind = Kind::Local;
    SkillResourceDescriptor resource;
    std::string capability_id;
    std::string registered_name;
    std::string source_name;
    std::string remote_name;
    std::shared_ptr<McpSession> mcp_session;
    ToolSideEffect side_effect = ToolSideEffect::Unknown;
};

std::string skill_capability_name(std::string_view skill_id,
                                  std::string_view capability_id) {
    if (skill_id.empty() || capability_id.empty())
        throw std::invalid_argument("skill and capability ids must not be empty");
    return "skill::" + std::string(skill_id) + "::" + std::string(capability_id);
}

SkillCapabilityBinding::SkillCapabilityBinding(
    SkillIndexEntry entry, std::shared_ptr<const SkillManifest> manifest,
    std::shared_ptr<SkillRuntime> runtime,
    std::shared_ptr<ToolBus> toolbus, SkillInvocationContext context,
    SkillMcpClientFactory mcp_factory)
    : entry_(std::move(entry)), manifest_(std::move(manifest)),
      runtime_(std::move(runtime)), toolbus_(std::move(toolbus)), context_(std::move(context)),
      mcp_factory_(std::move(mcp_factory)) {}

SkillCapabilityBinding::~SkillCapabilityBinding() { close(); }

bool SkillCapabilityBinding::closed() const noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return closed_;
}

bool SkillCapabilityBinding::acquire_call() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (closed_) return false;
    ++active_calls_;
    return true;
}

void SkillCapabilityBinding::release_call() const noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (active_calls_ > 0) --active_calls_;
    state_cv_.notify_all();
}

void SkillCapabilityBinding::close(std::chrono::milliseconds timeout) noexcept {
    bool cancel_active = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (closed_) return;
        closed_ = true;
        cancel_active = active_calls_ > 0;
    }
    toolbus_->unregister_tools(registered_tools_);
    if (activated_ && cancel_active && context_.control) context_.control->request_cancel();
    {
        std::unique_lock<std::mutex> lock(state_mutex_);
        (void)state_cv_.wait_for(lock, timeout, [&] { return active_calls_ == 0; });
    }
    for (const auto& session : mcp_sessions_) {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->client) session->client->disconnect();
    }
}

json SkillCapabilityBinding::invoke(std::size_t index, const json& arguments,
                                    const ToolCallControl& control) {
    if (!acquire_call()) return failure(kSkillDependencyUnavailable, "skill binding is closed");
    struct Release {
        const SkillCapabilityBinding* binding;
        ~Release() { binding->release_call(); }
    } release{this};
    if (index >= capabilities_.size())
        return failure(kSkillDependencyUnavailable, "skill capability is unavailable");
    const auto capability = capabilities_[index];
    ToolCallControl effective_control = control;
    const auto external_cancel = control.cancellation_requested;
    const auto task_control = context_.control;
    effective_control.cancellation_requested = [external_cancel, task_control] {
        if (external_cancel) {
            try {
                if (external_cancel()) return true;
            } catch (...) {
                return true;
            }
        }
        if (!task_control) return false;
        task_control->check_deadline_now();
        return task_control->is_cancel_requested() || task_control->is_deadline_exceeded();
    };
    auto begun = runtime_->begin_snapshot(entry_, manifest_, capability->resource.id,
                                           capability->resource.kind, arguments, context_);
    if (!begun.ok || !begun.ticket) return begun.error;
    const auto declared = begun.ticket->policy->authorize_tool(capability->registered_name);
    if (!declared.allowed) return SkillRuntime::permission_error(declared);
    if (effective_control.should_stop()) {
        const bool timed_out = task_control && task_control->is_deadline_exceeded();
        runtime_->record_termination(*begun.ticket, timed_out);
        return failure(kSkillCancelled, "skill capability invocation cancelled");
    }

    json output;
    if (capability->kind == Capability::Kind::Local) {
        const auto source = begun.ticket->policy->authorize_tool(capability->source_name);
        if (!source.allowed) return SkillRuntime::permission_error(source);
        output = toolbus_->call_tool(capability->source_name, arguments, effective_control).get();
    } else {
        std::shared_ptr<MCPClient> client;
        {
            std::lock_guard<std::mutex> lock(capability->mcp_session->mutex);
            if (!capability->mcp_session->client)
                capability->mcp_session->client = mcp_factory_(
                    capability->mcp_session->descriptor, context_);
            client = capability->mcp_session->client;
        }
        output = client->call_tool(capability->remote_name, arguments,
                                   effective_control.cancellation_requested).get();
    }
    if (effective_control.should_stop()) {
        const bool timed_out = task_control && task_control->is_deadline_exceeded();
        runtime_->record_termination(*begun.ticket, timed_out);
        return failure(kSkillCancelled, "skill capability invocation cancelled");
    }
    auto finished = runtime_->finish(*begun.ticket, output);
    return finished.ok ? output : finished.error;
}

json SkillCapabilityBinding::invoke_capability(
    const std::string& capability_id, const json& arguments,
    const ToolCallControl& control) {
    const auto found = std::find_if(
        capabilities_.begin(), capabilities_.end(),
        [&](const std::shared_ptr<Capability>& capability) {
            return capability->capability_id == capability_id;
        });
    if (found == capabilities_.end())
        return failure(kSkillDependencyUnavailable, "skill capability is unavailable",
                       {{"capability_id", capability_id}});
    return invoke(static_cast<std::size_t>(std::distance(capabilities_.begin(), found)),
                  arguments, control);
}

std::optional<ToolSideEffect> SkillCapabilityBinding::capability_side_effect(
    const std::string& capability_id) const {
    const auto found = std::find_if(
        capabilities_.begin(), capabilities_.end(),
        [&](const std::shared_ptr<Capability>& capability) {
            return capability->capability_id == capability_id;
        });
    return found == capabilities_.end()
        ? std::nullopt : std::optional<ToolSideEffect>((*found)->side_effect);
}

SkillPromptResult SkillCapabilityBinding::render_prompt(
    const std::string& resource_id, const SkillPromptSources& sources) const {
    if (!acquire_call()) return {false, {}, failure(kSkillDependencyUnavailable, "skill binding is closed")};
    struct Release {
        const SkillCapabilityBinding* binding;
        ~Release() { binding->release_call(); }
    } release{this};
    const auto found = std::find_if(manifest_->resources.begin(), manifest_->resources.end(),
                                    [&](const SkillResourceDescriptor& resource) {
        return resource.id == resource_id &&
               (resource.kind == SkillResourceType::Prompt ||
                resource.kind == SkillResourceType::Template);
    });
    if (found == manifest_->resources.end())
        return {false, {}, failure(kSkillDependencyUnavailable, "prompt resource is unavailable")};
    const auto kind = found->kind;
    auto begun = runtime_->begin_snapshot(entry_, manifest_, resource_id, kind, sources.input,
                                           context_);
    if (!begun.ok || !begun.ticket) return {false, {}, begun.error};
    const auto package = entry_.script_jail.value_or(entry_.file_path.parent_path());
    const auto read = begun.ticket->policy->authorize_filesystem(package / found->path, false);
    if (!read.allowed) return {false, {}, SkillRuntime::permission_error(read)};

    const auto descriptor_it = prompt_descriptors_.find(resource_id);
    if (descriptor_it == prompt_descriptors_.end())
        return {false, {}, failure(kSkillDescriptorInvalid, "prompt descriptor snapshot is unavailable",
                                   {{"resource_id", resource_id}})};
    const json& descriptor = descriptor_it->second;
    if (!descriptor.contains("template") || !descriptor.at("template").is_string())
        return {false, {}, failure(kSkillDescriptorInvalid, "template must be a string")};
    if (descriptor.contains("variables") && !descriptor.at("variables").is_object())
        return {false, {}, failure(kSkillDescriptorInvalid, "variables must be an object")};
    const std::size_t max_bytes = descriptor.value("max-bytes", found->size_limit.value_or(65536U));
    std::string rendered = descriptor.at("template").get<std::string>();
    const json variables = descriptor.value("variables", json::object());
    for (auto it = variables.begin(); it != variables.end(); ++it) {
        if (!it.value().is_object())
            return {false, {}, failure(kSkillDescriptorInvalid, "variable descriptor must be an object",
                                       {{"variable", it.key()}})};
        const std::string source_name = it.value().value("source", std::string{});
        const json* source = nullptr;
        if (source_name == "input") source = &sources.input;
        else if (source_name == "context") source = &sources.context;
        else if (source_name == "task") source = &sources.task;
        else return {false, {}, failure(kSkillPromptSourceDenied,
                                        "prompt variable source is not allowed",
                                        {{"variable", it.key()}, {"source", source_name}})};
        const std::string pointer = it.value().value("path", "/" + it.key());
        bool exists = false;
        std::string value = pointer_value(*source, pointer, exists);
        if (!exists && it.value().value("required", true))
            return {false, {}, failure(kSkillPromptVariableMissing,
                                       "required prompt variable is missing",
                                       {{"variable", it.key()}, {"source", source_name},
                                        {"path", pointer}})};
        if (exists && it.value().contains("schema")) {
            json validation_error;
            const json& raw = pointer.empty() ? *source : source->at(json::json_pointer(pointer));
            if (!validate_json_instance(it.value().at("schema"), raw, validation_error))
                return {false, {}, failure(kSkillInputInvalid, "prompt variable schema failed",
                                           validation_error.value("details", json::object()))};
        }
        replace_all(rendered, "{{" + it.key() + "}}", value);
    }
    if (rendered.find("{{") != std::string::npos)
        return {false, {}, failure(kSkillPromptVariableMissing,
                                   "template contains an undeclared or missing variable")};
    if (rendered.size() > max_bytes)
        return {false, {}, failure(kSkillPromptSizeExceeded, "rendered prompt exceeds byte limit",
                                   {{"bytes", rendered.size()}, {"limit", max_bytes}})};
    auto finished = runtime_->finish(*begun.ticket, rendered);
    return finished.ok ? SkillPromptResult{true, std::move(rendered), json::object()}
                       : SkillPromptResult{false, {}, finished.error};
}

SkillCapabilityRuntime::SkillCapabilityRuntime(
    std::shared_ptr<SkillRegistry> registry, std::shared_ptr<SkillLoader> loader,
    std::shared_ptr<SkillRuntime> runtime, std::shared_ptr<ToolBus> toolbus,
    SkillMcpClientFactory mcp_factory)
    : registry_(std::move(registry)), loader_(std::move(loader)), runtime_(std::move(runtime)),
      toolbus_(std::move(toolbus)),
      mcp_factory_(mcp_factory ? std::move(mcp_factory) : default_mcp_factory) {
    if (!registry_ || !loader_ || !runtime_ || !toolbus_)
        throw std::invalid_argument("SkillCapabilityRuntime requires all services");
}

SkillCapabilityBindResult SkillCapabilityRuntime::bind(
    const std::string& skill_id, SkillInvocationContext context,
    SkillCapabilityBindOptions options) const {
    const auto registry_snapshot = registry_->snapshot();
    const auto entry = registry_snapshot.get(skill_id);
    const auto manifest = registry_snapshot.get_manifest(skill_id);
    if (!entry || !manifest)
        return {nullptr, failure(kSkillDependencyUnavailable, "skill is not available",
                                 {{"skill_id", skill_id}})};
    return bind_snapshot(*entry, manifest, std::move(context), options);
}

SkillCapabilityBindResult SkillCapabilityRuntime::bind_snapshot(
    const SkillIndexEntry& entry, std::shared_ptr<const SkillManifest> manifest,
    SkillInvocationContext context, SkillCapabilityBindOptions options) const {
    if (!manifest || entry.id.empty())
        return {nullptr, failure(kSkillDependencyUnavailable, "skill snapshot is unavailable")};
    const std::string& skill_id = entry.id;
    auto binding = std::shared_ptr<SkillCapabilityBinding>(new SkillCapabilityBinding(
        entry, manifest, runtime_, toolbus_, std::move(context), mcp_factory_));
    std::vector<ToolBus::AtomicLocalToolRegistration> registrations;
    try {
        SkillPolicyEngine policy(manifest->permissions, binding->context_.grants,
                                 entry.script_jail.value_or(entry.file_path.parent_path()));
        for (const auto& resource : manifest->resources) {
            if (resource.kind == SkillResourceType::Prompt ||
                resource.kind == SkillResourceType::Template) {
                json prompt_descriptor = load_descriptor(entry, resource, loader_);
                validate_prompt_descriptor(prompt_descriptor);
                binding->prompt_descriptors_.emplace(resource.id, std::move(prompt_descriptor));
                continue;
            }
            if (resource.kind != SkillResourceType::Tool && resource.kind != SkillResourceType::Mcp)
                continue;
            const json descriptor = load_descriptor(entry, resource, loader_);
            if (resource.kind == SkillResourceType::Tool) {
                const std::string source = descriptor.value("source", std::string{});
                if (source.empty()) throw std::runtime_error("Tool descriptor source is required");
                ToolMeta meta = toolbus_->get_tool_meta(source);
                if (meta.name.empty()) throw std::runtime_error("imported Tool is unavailable: " + source);
                auto capability = std::make_shared<SkillCapabilityBinding::Capability>();
                capability->resource = resource;
                capability->capability_id = resource.id;
                capability->registered_name = skill_capability_name(skill_id, resource.id);
                capability->source_name = source;
                capability->kind = SkillCapabilityBinding::Capability::Kind::Local;
                capability->side_effect = meta.side_effect;
                meta.name = capability->registered_name;
                meta.llm_visible = descriptor.value("export", false);
                const std::size_t index = binding->capabilities_.size();
                binding->capabilities_.push_back(capability);
                registrations.push_back({capability->registered_name,
                    [weak = std::weak_ptr<SkillCapabilityBinding>(binding), index]
                    (const json& args, const ToolCallControl& control) {
                        auto locked = weak.lock();
                        return locked ? locked->invoke(index, args, control)
                                      : failure(kSkillDependencyUnavailable, "skill binding expired");
                    }, meta});
            } else {
                const SkillMcpDescriptor mcp = skill_parse_mcp_descriptor(descriptor);
                if (mcp.transport == "http") {
                    const auto decision = policy.authorize_network(mcp.url);
                    if (!decision.allowed)
                        return {nullptr, SkillRuntime::permission_error(decision)};
                }
                for (const auto& [target, reference] : mcp.secret_references) {
                    (void)target;
                    const auto decision = policy.authorize_secret(reference);
                    if (!decision.allowed)
                        return {nullptr, SkillRuntime::permission_error(decision)};
                }
                std::shared_ptr<MCPClient> eager;
                std::vector<ToolMeta> discovered;
                if (mcp.startup == "eager") {
                    eager = mcp_factory_(mcp, binding->context_);
                    discovered = eager->list_tools().get();
                }
                auto session = std::make_shared<SkillCapabilityBinding::McpSession>();
                session->descriptor = mcp;
                session->client = std::move(eager);
                binding->mcp_sessions_.push_back(session);
                std::map<std::string, SkillMcpToolSpec> declared;
                for (const auto& tool : mcp.tools) declared[tool.name] = tool;
                if (declared.empty())
                    for (const auto& tool : discovered) declared[tool.name] = {tool.name, false};
                for (const auto& [remote_name, spec] : declared) {
                    if (!selected(mcp.tool_filters, remote_name)) continue;
                    ToolMeta meta;
                    const auto found = std::find_if(discovered.begin(), discovered.end(),
                                                    [&](const ToolMeta& value) {
                        return value.name == remote_name;
                    });
                    if (found != discovered.end()) meta = *found;
                    else if (mcp.startup == "eager")
                        throw std::runtime_error("declared MCP tool is unavailable: " + remote_name);
                    else meta.schema = json{{"type", "object"}, {"properties", json::object()}};
                    auto capability = std::make_shared<SkillCapabilityBinding::Capability>();
                    capability->resource = resource;
                    capability->capability_id = resource.id + "." + remote_name;
                    capability->registered_name = skill_capability_name(
                        skill_id, resource.id + "." + remote_name);
                    capability->remote_name = remote_name;
                    capability->mcp_session = session;
                    capability->kind = SkillCapabilityBinding::Capability::Kind::Mcp;
                    meta.name = capability->registered_name;
                    meta.llm_visible = spec.exported;
                    const std::size_t index = binding->capabilities_.size();
                    binding->capabilities_.push_back(capability);
                    registrations.push_back({capability->registered_name,
                        [weak = std::weak_ptr<SkillCapabilityBinding>(binding), index]
                        (const json& args, const ToolCallControl& control) {
                            auto locked = weak.lock();
                            return locked ? locked->invoke(index, args, control)
                                          : failure(kSkillDependencyUnavailable, "skill binding expired");
                        }, meta});
                }
            }
        }
        std::vector<std::string> published_names;
        for (const auto& registration : registrations)
            published_names.push_back(registration.name);
        if (options.publish_to_toolbus) {
            toolbus_->register_local_tools_atomic(std::move(registrations));
            binding->registered_tools_ = std::move(published_names);
        }
        binding->activated_ = true;
        return {std::move(binding), json::object()};
    } catch (const std::exception& error) {
        const std::string message = error.what();
        const char* code = message.find(kSkillCapabilityConflict) != std::string::npos
                               ? kSkillCapabilityConflict : kSkillDescriptorInvalid;
        return {nullptr, failure(code, message, {{"skill_id", skill_id}})};
    }
}

} // namespace agent_framework
