/**
 * @file toolbus.cpp
 * @brief ToolBus implementation (WP1.2, WP2.1d hooks)
 */

#include <agent/toolbus/toolbus.hpp>

#include <agent/mcp_client/mcp_client.hpp>
#include <agent/context_budget/context_budget.hpp>
#include <agent/toolbus/schema_validate.hpp>

#include <cctype>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <optional>
#include <unordered_set>

namespace agent_framework {
namespace {

std::future<json> make_ready_json_future(json j) {
    std::promise<json> p;
    p.set_value(std::move(j));
    return p.get_future();
}

void trim_inplace(std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
}

std::optional<std::unordered_set<std::string>> g_allowlist_cache;
std::once_flag g_allowlist_once;

void load_allowlist_once() {
    std::call_once(g_allowlist_once, [] {
        const char* raw = std::getenv("AGENT_TOOL_ALLOWLIST");
        if (raw == nullptr || raw[0] == '\0') {
            g_allowlist_cache = std::nullopt;
            return;
        }
        std::unordered_set<std::string> set;
        std::string chunk;
        for (const char* p = raw; *p != '\0'; ++p) {
            if (*p == ',') {
                trim_inplace(chunk);
                if (!chunk.empty()) {
                    set.insert(std::move(chunk));
                    chunk.clear();
                }
            } else {
                chunk.push_back(*p);
            }
        }
        trim_inplace(chunk);
        if (!chunk.empty()) {
            set.insert(std::move(chunk));
        }
        g_allowlist_cache = std::move(set);
    });
}

const std::optional<std::unordered_set<std::string>>& allowlist() {
    load_allowlist_once();
    return g_allowlist_cache;
}

std::string portable_builtin_name(std::string_view raw) {
    static const std::map<std::string_view, std::string_view> aliases = {
        {"fs_read","Read"},{"fs_write","Write"},{"fs_replace","Edit"},
        {"fs_search","Glob"},{"fs_grep","Grep"},{"web_fetch","WebFetch"},
        {"fs_list_dir","LS"},{"cat","Cat"},{"ls","LS"},{"sed","Sed"},
        {"fs_mkdir","Mkdir"},{"mkdir","Mkdir"},{"fs_touch","Touch"},{"touch","Touch"},
        {"fs_delete","Remove"},{"rm","Remove"},{"remove","Remove"},
        {"web_search","WebSearch"},{"expr_eval","Calculate"},
        {"expr_validate","ValidateExpression"},{"expr_batch_eval","BatchCalculate"},
        {"draw_render","RenderChart"},{"draw_export","ExportChart"},
        {"python3","Python"},{"Python3","Python"}
        ,{"bash","Bash"},{"curl","Curl"},{"wget","Wget"},{"cmake","CMake"},{"make","Make"}
    };
    if (const auto it = aliases.find(raw); it != aliases.end()) return std::string(it->second);
    return std::string(raw);
}

bool is_tool_allowed(const std::string& name) {
    const auto& al = allowlist();
    if (!al.has_value()) {
        return true;
    }
    const auto normalized = portable_builtin_name(name);
    return std::any_of(al->begin(), al->end(), [&](const std::string& allowed) {
        return portable_builtin_name(allowed) == normalized;
    });
}

bool env_hook_throw_abort() {
    const char* v = std::getenv("AGENT_TOOL_HOOK_THROW_ABORT");
    if (v == nullptr || v[0] == '\0') {
        return false;
    }
    std::string s;
    s.reserve(std::strlen(v));
    for (const char* p = v; *p != '\0'; ++p) {
        s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
    }
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

std::string truncate_utf8_chars(const char* what, std::size_t max_bytes) {
    if (what == nullptr) {
        return {};
    }
    return utf8_safe_truncate(what, max_bytes);
}

/**
 * Runs hook chain on `current` (copy-in-out). Returns error object if short-circuited.
 */
std::optional<json> run_tool_call_hooks(const std::string& name, json& current,
                                        const std::vector<ToolCallHook>& hooks_copy) {
    for (std::size_t i = 0; i < hooks_copy.size(); ++i) {
        try {
            ToolHookResult r = hooks_copy[i](name, current);
            switch (r.verdict) {
            case ToolHookVerdict::Allow:
                break;
            case ToolHookVerdict::Deny: {
                const std::string msg = r.deny_message.empty() ? "hook denied" : r.deny_message;
                json det = r.deny_details.is_object() ? r.deny_details : json::object();
                det["hook_index"] = i;
                return json{{"error", msg}, {"code", "hook_denied"}, {"details", std::move(det)}};
            }
            case ToolHookVerdict::Replace:
                if (!r.replaced_arguments.has_value() || !r.replaced_arguments->is_object()) {
                    json det = json::object();
                    det["hook_index"] = i;
                    return json{{"error", "invalid hook Replace arguments"},
                                 {"code", "hook_invalid_replace"},
                                 {"details", std::move(det)}};
                }
                current = std::move(*r.replaced_arguments);
                break;
            }
        } catch (const std::exception& e) {
            if (env_hook_throw_abort()) {
                throw;
            }
            json det = json::object();
            det["hook_index"] = i;
            det["exception"] = truncate_utf8_chars(e.what(), 512);
            return json{{"error", "tool call hook threw an exception"},
                        {"code", "hook_threw"},
                        {"details", std::move(det)}};
        } catch (...) {
            if (env_hook_throw_abort()) {
                throw;
            }
            json det = json::object();
            det["hook_index"] = i;
            det["exception"] = "non-standard exception";
            return json{{"error", "tool call hook threw an exception"},
                        {"code", "hook_threw"},
                        {"details", std::move(det)}};
        }
    }
    return std::nullopt;
}

std::string env_or_empty(const char* key) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : std::string();
}

std::string default_cursor_mcp_path() {
    std::string from_env = env_or_empty("AGENT_MCP_CONFIG_PATH");
    if (!from_env.empty()) {
        return from_env;
    }
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return std::string(home) + "/.cursor/mcp.json";
    }
    return "/home/mapoet/.cursor/mcp.json";
}

json read_json_file_or_throw(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("failed to open mcp config: " + path);
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return json::parse(ss.str());
}

std::string mcp_config_parent_abs(const std::string& config_path) {
    namespace fs = std::filesystem;
    fs::path p(config_path);
    fs::path dir = p.has_parent_path() ? p.parent_path() : fs::path(".");
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(fs::absolute(dir), ec);
    if (ec) {
        canon = fs::absolute(dir);
    }
    return canon.string();
}

void expand_mcp_stdio_args(std::vector<std::string>& args, const std::string& config_parent_abs) {
    const std::vector<std::pair<std::string, std::string>> replacements = {
        {"${CONFIG_DIR}", config_parent_abs},
        {"${AGENT_FS_ROOT}", env_or_empty("AGENT_FS_ROOT")},
    };
    for (std::string& a : args) {
        for (const auto& replacement : replacements) {
            if (replacement.second.empty() && a.find(replacement.first) != std::string::npos) {
                throw std::invalid_argument(replacement.first + " is used but its environment variable is unset");
            }
            std::size_t pos = 0;
            while ((pos = a.find(replacement.first, pos)) != std::string::npos) {
                a.replace(pos, replacement.first.size(), replacement.second);
                pos += replacement.second.size();
            }
        }
    }
}

std::vector<std::string> parse_string_array(const json& arr) {
    std::vector<std::string> out;
    if (!arr.is_array()) {
        return out;
    }
    for (const auto& a : arr) {
        if (a.is_string()) {
            out.push_back(a.get<std::string>());
        }
    }
    return out;
}

std::map<std::string, std::string> parse_string_map(const json& obj) {
    std::map<std::string, std::string> out;
    if (!obj.is_object()) {
        return out;
    }
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (it.value().is_string()) {
            out[it.key()] = it.value().get<std::string>();
        }
    }
    return out;
}

MCPStdioFraming parse_stdio_framing(const json& server) {
    if (!server.contains("framing")) {
        return MCPStdioFraming::JsonLines;
    }
    if (!server["framing"].is_string()) {
        throw std::invalid_argument("stdio framing must be a string");
    }
    const std::string framing = server["framing"].get<std::string>();
    if (framing == "jsonl") {
        return MCPStdioFraming::JsonLines;
    }
    if (framing == "content-length") {
        return MCPStdioFraming::ContentLength;
    }
    throw std::invalid_argument(
        "unknown stdio framing (expected jsonl or content-length)");
}

} // namespace

std::shared_ptr<ToolInterface> ToolBus::find_tool(const std::string& name) const {
    std::lock_guard<std::mutex> lock(tools_mutex_);
    std::string resolved = name;
    if (const auto alias = aliases_.find(name); alias != aliases_.end()) resolved = alias->second;
    auto it = tools_.find(resolved);
    if (it == tools_.end()) {
        return nullptr;
    }
    return it->second;
}

void ToolBus::register_tool_alias(const std::string& alias, const std::string& canonical_name) {
    register_tool_alias({alias,canonical_name,"AT-V2","next-major"});
}
void ToolBus::register_tool_alias(const ToolAliasInfo& info) {
    const auto&alias=info.alias;const auto&canonical_name=info.canonical_name;
    if (alias.empty() || canonical_name.empty() || alias == canonical_name) {
        throw std::invalid_argument("register_tool_alias: invalid alias");
    }
    std::lock_guard<std::mutex> lock(tools_mutex_);
    if (tools_.count(alias) || aliases_.count(alias)) {
        throw std::invalid_argument("register_tool_alias: name already registered: " + alias);
    }
    if (!tools_.count(canonical_name)) {
        throw std::invalid_argument("register_tool_alias: unknown canonical tool: " + canonical_name);
    }
    aliases_.emplace(alias, canonical_name);
    alias_metadata_.emplace(alias,info);
}

std::optional<ToolBus::ToolAliasInfo> ToolBus::alias_info(std::string_view alias) const {std::lock_guard<std::mutex>lock(tools_mutex_);auto it=alias_metadata_.find(std::string(alias));return it==alias_metadata_.end()?std::nullopt:std::optional<ToolAliasInfo>(it->second);}

std::string ToolBus::resolve_tool_name(std::string_view requested_name) const {
    std::lock_guard<std::mutex> lock(tools_mutex_);
    const std::string requested(requested_name);
    if (const auto alias = aliases_.find(requested); alias != aliases_.end()) return alias->second;
    return requested;
}

void ToolBus::register_local_tool(const std::string& name,
                                  std::function<json(const json&)> func, const ToolMeta& meta) {
    load_allowlist_once();
    if (!is_tool_allowed(name)) {
        throw std::invalid_argument("tool name not in AGENT_TOOL_ALLOWLIST: " + name);
    }
    std::lock_guard<std::mutex> lock(tools_mutex_);
    if (tools_.count(name) != 0U) {
        throw std::invalid_argument("register_local_tool: tool already registered: " + name);
    }
    tools_.emplace(name, std::make_shared<LocalTool>(name, std::move(func), meta));
}

void ToolBus::register_cancellable_local_tool(
    const std::string& name,
    std::function<json(const json&, const ToolCallControl&)> func,
    const ToolMeta& meta) {
    load_allowlist_once();
    if (!is_tool_allowed(name)) {
        throw std::invalid_argument("tool name not in AGENT_TOOL_ALLOWLIST: " + name);
    }
    std::lock_guard<std::mutex> lock(tools_mutex_);
    if (tools_.count(name) != 0U) {
        throw std::invalid_argument("register_cancellable_local_tool: tool already registered: " + name);
    }
    tools_.emplace(name, std::make_shared<LocalTool>(name, std::move(func), meta));
}

void ToolBus::register_local_tools_atomic(
    std::vector<AtomicLocalToolRegistration> registrations) {
    load_allowlist_once();
    std::unordered_set<std::string> incoming;
    for (const auto& registration : registrations) {
        if (registration.name.empty() || !registration.function) {
            throw std::invalid_argument("register_local_tools_atomic: invalid registration");
        }
        if (!incoming.insert(registration.name).second) {
            throw std::invalid_argument("skill_capability_conflict: duplicate incoming tool: " +
                                        registration.name);
        }
        if (!is_tool_allowed(registration.name)) {
            throw std::invalid_argument("tool name not in AGENT_TOOL_ALLOWLIST: " +
                                        registration.name);
        }
    }
    std::lock_guard<std::mutex> lock(tools_mutex_);
    for (const auto& registration : registrations) {
        if (tools_.count(registration.name) != 0U) {
            throw std::invalid_argument("skill_capability_conflict: tool already registered: " +
                                        registration.name);
        }
    }
    for (auto& registration : registrations) {
        tools_.emplace(registration.name,
                       std::make_shared<LocalTool>(registration.name,
                                                   std::move(registration.function),
                                                   registration.meta));
    }
}

void ToolBus::unregister_tools(const std::vector<std::string>& names) noexcept {
    try {
        std::lock_guard<std::mutex> lock(tools_mutex_);
        for (const auto& name : names) {
            tools_.erase(name);
            aliases_.erase(name);
            for (auto it = aliases_.begin(); it != aliases_.end();) {
                if (it->second == name) it = aliases_.erase(it); else ++it;
            }
        }
    } catch (...) {
        // Lifecycle cleanup must not throw from destructors.
    }
}

void ToolBus::ensure_default_tools_registered(const std::function<void()>& registrar) {
    if (!registrar) {
        throw std::invalid_argument("default tool registrar is empty");
    }
    std::lock_guard<std::mutex> lock(default_tools_mutex_);
    if (default_tools_registered_) return;
    registrar();
    default_tools_registered_ = true;
}

void ToolBus::register_mcp_service(const std::string& service_name, std::shared_ptr<MCPClient> client) {
    if (service_name.empty()) {
        throw std::invalid_argument("register_mcp_service: empty service_name");
    }
    if (!client) {
        throw std::invalid_argument("register_mcp_service: null MCPClient");
    }
    if (!client->is_connected()) {
        throw std::invalid_argument("register_mcp_service: MCPClient not connected");
    }
    load_allowlist_once();
    const auto& al = allowlist();
    std::vector<ToolMeta> metas;
    if (client->supports_tools() || !client->supports_resources()) {
        // Keep the historical fallback for pre-capability MCP tool servers while allowing
        // standards-compliant resource-only servers to register without tools/list.
        metas = client->list_tools().get();
    }
    for (const auto& m : metas) {
        const std::string reg = service_name + "__" + m.name;
        if (al.has_value() && al->count(reg) == 0U) {
            throw std::invalid_argument("AGENT_TOOL_ALLOWLIST blocks MCP tool: " + reg);
        }
    }
    std::lock_guard<std::mutex> lock(tools_mutex_);
    if (mcp_clients_.count(service_name) != 0U) {
        throw std::invalid_argument("register_mcp_service: service already registered: " + service_name);
    }
    for (const auto& m : metas) {
        const std::string reg = service_name + "__" + m.name;
        if (tools_.count(reg) != 0U) {
            throw std::invalid_argument("register_mcp_service: tool name conflict: " + reg);
        }
    }
    for (const auto& m : metas) {
        const std::string reg = service_name + "__" + m.name;
        ToolMeta tm = m;
        tm.name = reg;
        if (tm.schema.is_null() || tm.schema.empty()) {
            tm.schema = json{{"type", "object"}, {"properties", json::object()}};
        }
        tools_.emplace(reg, std::make_shared<MCPProxyTool>(client, reg, m.name, tm));
    }
    mcp_clients_.emplace(service_name, std::move(client));
}

void ToolBus::unregister_mcp_service(const std::string& service_name) noexcept {
    try {
        std::lock_guard<std::mutex> lock(tools_mutex_);
        mcp_clients_.erase(service_name);
        const std::string prefix = service_name + "__";
        for (auto it = tools_.begin(); it != tools_.end();) {
            if (it->first.rfind(prefix, 0) == 0) it = tools_.erase(it);
            else ++it;
        }
    } catch (...) {}
}

bool ToolBus::has_mcp_service(std::string_view service_name) const {
    std::lock_guard<std::mutex> lock(tools_mutex_);
    return mcp_clients_.count(std::string(service_name)) != 0U;
}

std::future<MCPResourceListResult> ToolBus::list_mcp_resources(
    const std::string& service_name, std::string cursor,
    std::function<bool()> cancellation_requested) const {
    std::shared_ptr<MCPClient> client;
    {
        std::lock_guard<std::mutex> lock(tools_mutex_);
        const auto it = mcp_clients_.find(service_name);
        if (it == mcp_clients_.end())
            throw std::runtime_error("mcp_resource_service_unknown:" + service_name);
        client = it->second;
    }
    return client->list_resources(std::move(cursor), std::move(cancellation_requested));
}

std::future<std::vector<MCPResourceContent>> ToolBus::read_mcp_resource(
    const std::string& service_name, std::string uri,
    std::function<bool()> cancellation_requested) const {
    std::shared_ptr<MCPClient> client;
    {
        std::lock_guard<std::mutex> lock(tools_mutex_);
        const auto it = mcp_clients_.find(service_name);
        if (it == mcp_clients_.end())
            throw std::runtime_error("mcp_resource_service_unknown:" + service_name);
        client = it->second;
    }
    return client->read_resource(std::move(uri), std::move(cancellation_requested));
}

void ToolBus::register_api_tool(const std::string& /*name*/, const std::string& /*endpoint*/,
                                const std::string& /*method*/, const ToolMeta& /*meta*/) {
    throw std::logic_error("WP1.3: API tool not implemented (register_api_tool)");
}

void ToolBus::add_tool_call_hook(ToolCallHook hook) {
    if (!hook) {
        throw std::invalid_argument("add_tool_call_hook: hook is empty");
    }
    std::lock_guard<std::mutex> lock(hooks_mutex_);
    hooks_.push_back(std::move(hook));
}

void ToolBus::clear_tool_call_hooks() {
    std::lock_guard<std::mutex> lock(hooks_mutex_);
    hooks_.clear();
}

std::size_t ToolBus::tool_call_hook_count() const {
    std::lock_guard<std::mutex> lock(hooks_mutex_);
    return hooks_.size();
}

std::future<json> ToolBus::call_tool(const std::string& name, const json& arguments,
                                     const ToolCallControl& control) {
    if (control.should_stop()) {
        return make_ready_json_future(json{{"error", "tool call cancelled"}, {"code", "cancelled"}});
    }
    load_allowlist_once();
    const std::string resolved_name = resolve_tool_name(name);
    if(const auto migration=alias_info(name)) {
        const char* strict=std::getenv("AGENT_REJECT_LEGACY_TOOL_ALIASES");
        if(strict&&std::string(strict)!="0"&&std::string(strict)!="false")
            return make_ready_json_future(json{{"error","legacy tool alias rejected"},{"code","legacy_tool_alias_rejected"},{"details",{{"requested_name",name},{"canonical_name",resolved_name},{"deprecated_since",migration->deprecated_since},{"removal_target",migration->removal_target},{"permission_expanded",false}}}});
        if(control.migration_diagnostic)control.migration_diagnostic({{"code","legacy_tool_alias_used"},{"requested_name",name},{"canonical_name",resolved_name},{"deprecated_since",migration->deprecated_since},{"removal_target",migration->removal_target},{"permission_expanded",false}});
    }
    auto tool = find_tool(resolved_name);
    if (tool == nullptr) {
        return make_ready_json_future(json{{"error", "unknown tool: " + name},
                                            {"code", "unknown_tool"},
                                            {"details", json{{"name", name}}}});
    }
    if (!is_tool_allowed(name) && !is_tool_allowed(resolved_name)) {
        return make_ready_json_future(json{{"error", "tool not allowed by AGENT_TOOL_ALLOWLIST"},
                                            {"code", "tool_not_allowed"},
                                            {"details", json{{"name", name}}}});
    }
    ToolMeta tm = tool->get_tool_meta(resolved_name);
    const auto authorize = [&](const json& candidate) -> std::optional<json> {
        if (!control.authorization) return std::nullopt;
        try {
            return control.authorization(resolved_name, candidate, tm);
        } catch (const std::exception& error) {
            return json{{"error", "tool authorization failed"},
                        {"code", "skill_permission_denied"},
                        {"details", json{{"name", name},
                                         {"reason", truncate_utf8_chars(error.what(), 512)}}}};
        } catch (...) {
            return json{{"error", "tool authorization failed"},
                        {"code", "skill_permission_denied"},
                        {"details", json{{"name", name},
                                         {"reason", "non-standard exception"}}}};
        }
    };
    if (auto denied = authorize(arguments)) {
        return make_ready_json_future(std::move(*denied));
    }

    std::vector<ToolCallHook> hooks_copy;
    {
        std::lock_guard<std::mutex> lock(hooks_mutex_);
        hooks_copy = hooks_;
    }

    json current = arguments;
    if (!hooks_copy.empty()) {
        if (auto hook_err = run_tool_call_hooks(resolved_name, current, hooks_copy)) {
            return make_ready_json_future(std::move(*hook_err));
        }
    }

    if (current != arguments) {
        if (auto denied = authorize(current)) {
            return make_ready_json_future(std::move(*denied));
        }
    }

    json err = json::object();
    const json& schema = tm.schema;
    if (!validate_tool_arguments(schema, current, err)) {
        return make_ready_json_future(std::move(err));
    }
    return tool->call_cancellable(resolved_name, current, control);
}

std::vector<ToolMeta> ToolBus::export_as_llm_tools(
    const std::function<bool(std::string_view)>& filter) const {
    std::lock_guard<std::mutex> lock(tools_mutex_);
    std::vector<ToolMeta> out;
    out.reserve(tools_.size());
    for (const auto& kv : tools_) {
        if (filter && !filter(kv.first)) continue;
        ToolMeta m = kv.second->get_tool_meta(kv.first);
        if (!m.name.empty() && m.llm_visible) {
            out.push_back(std::move(m));
        }
    }
    return out;
}

ToolMeta ToolBus::get_tool_meta(const std::string& name) const {
    const std::string resolved = resolve_tool_name(name);
    auto tool = find_tool(resolved);
    if (tool == nullptr) {
        return ToolMeta{};
    }
    return tool->get_tool_meta(resolved);
}

std::optional<ToolInfo> ToolBus::get_tool_info(const std::string& name) const {
    const std::string resolved = resolve_tool_name(name);
    auto tool = find_tool(resolved);
    if (tool == nullptr) {
        return std::nullopt;
    }
    return tool->get_tool_info(resolved);
}

std::vector<std::string> ToolBus::list_all_tools() const {
    std::lock_guard<std::mutex> lock(tools_mutex_);
    std::vector<std::string> names;
    names.reserve(tools_.size());
    for (const auto& kv : tools_) {
        names.push_back(kv.first);
    }
    return names;
}

ToolBus::CursorMcpImportResult ToolBus::register_mcp_from_cursor_config(const std::string& config_path,
                                                                        bool register_all,
                                                                        const std::vector<std::string>& skip_services) {
    (void)register_all;
    CursorMcpImportResult result;

    const std::string path = config_path.empty() ? default_cursor_mcp_path() : config_path;
    json doc;
    try {
        doc = read_json_file_or_throw(path);
    } catch (const std::exception& e) {
        result.failures.push_back(CursorMcpImportFailure{"<config>", e.what()});
        return result;
    }
    if (!doc.contains("mcpServers") || !doc["mcpServers"].is_object()) {
        result.failures.push_back(
            CursorMcpImportFailure{"<config>", "mcp.json missing object field mcpServers"});
        return result;
    }

    const std::string config_parent_abs = mcp_config_parent_abs(path);

    const json& servers = doc["mcpServers"];
    const std::unordered_set<std::string> skipped(skip_services.begin(), skip_services.end());
    for (auto it = servers.begin(); it != servers.end(); ++it) {
        const std::string service_name = it.key();
        const json& s = it.value();
        if (skipped.count(service_name) != 0U) {
            result.skipped_services.push_back(service_name);
            std::clog << "[ToolBus] MCP: skipping \"" << service_name << "\" by policy\n" << std::flush;
            continue;
        }
        if (!s.is_object()) {
            result.failures.push_back(
                CursorMcpImportFailure{service_name, "server entry must be a JSON object"});
            continue;
        }

        std::clog << "[ToolBus] MCP: connecting \"" << service_name << "\"...\n" << std::flush;

        try {
            std::shared_ptr<MCPClient> client;
            if (s.contains("url") && s["url"].is_string()) {
                std::string url = s["url"].get<std::string>();
                std::map<std::string, std::string> headers;
                if (s.contains("headers")) {
                    headers = parse_string_map(s["headers"]);
                }
                client = MCPClient::create_http(url, headers);
            } else if (s.contains("command") && s["command"].is_string()) {
                std::string command = s["command"].get<std::string>();
                std::vector<std::string> args;
                if (s.contains("args")) {
                    args = parse_string_array(s["args"]);
                    expand_mcp_stdio_args(args, config_parent_abs);
                }
                std::map<std::string, std::string> env;
                if (s.contains("env")) {
                    env = parse_string_map(s["env"]);
                }
                client = MCPClient::create_stdio(command, args, env, parse_stdio_framing(s));
            } else {
                throw std::invalid_argument("unknown server type (need url or command)");
            }

            register_mcp_service(service_name, client);
            result.registered_services.push_back(service_name);
            std::clog << "[ToolBus] MCP: \"" << service_name << "\" registered\n" << std::flush;
        } catch (const std::exception& e) {
            std::clog << "[ToolBus] MCP: \"" << service_name << "\" failed: " << e.what() << '\n'
                      << std::flush;
            result.failures.push_back(CursorMcpImportFailure{service_name, e.what()});
        }
    }

    return result;
}

} // namespace agent_framework
