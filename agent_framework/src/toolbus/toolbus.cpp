/**
 * @file toolbus.cpp
 * @brief ToolBus implementation (WP1.2, WP2.1d hooks)
 */

#include <agent/toolbus/toolbus.hpp>

#include <agent/mcp_client/mcp_client.hpp>
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

bool is_tool_allowed(const std::string& name) {
    const auto& al = allowlist();
    if (!al.has_value()) {
        return true;
    }
    return al->count(name) != 0U;
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

/** UTF-8 safe prefix: drop trailing continuation bytes if cut mid-codepoint */
std::string truncate_utf8_chars(const char* what, std::size_t max_bytes) {
    if (what == nullptr) {
        return {};
    }
    std::string_view sv(what);
    if (sv.size() <= max_bytes) {
        return std::string(sv);
    }
    std::size_t n = max_bytes;
    while (n > 0 && (static_cast<unsigned char>(sv[n - 1]) & 0xC0u) == 0x80u) {
        --n;
    }
    return std::string(sv.substr(0, n));
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
    constexpr const char* k_token = "${CONFIG_DIR}";
    for (std::string& a : args) {
        std::size_t pos = 0;
        while ((pos = a.find(k_token, pos)) != std::string::npos) {
            a.replace(pos, std::strlen(k_token), config_parent_abs);
            pos += config_parent_abs.size();
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
    auto it = tools_.find(name);
    if (it == tools_.end()) {
        return nullptr;
    }
    return it->second;
}

void ToolBus::register_local_tool(const std::string& name,
                                  std::function<json(const json&)> func, const ToolMeta& meta) {
    load_allowlist_once();
    const auto& al = allowlist();
    if (al.has_value() && al->count(name) == 0U) {
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
    const auto& al = allowlist();
    if (al.has_value() && al->count(name) == 0U) {
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
    const auto& al = allowlist();
    std::unordered_set<std::string> incoming;
    for (const auto& registration : registrations) {
        if (registration.name.empty() || !registration.function) {
            throw std::invalid_argument("register_local_tools_atomic: invalid registration");
        }
        if (!incoming.insert(registration.name).second) {
            throw std::invalid_argument("skill_capability_conflict: duplicate incoming tool: " +
                                        registration.name);
        }
        if (al.has_value() && al->count(registration.name) == 0U) {
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
        for (const auto& name : names) tools_.erase(name);
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
    auto tool = find_tool(name);
    if (tool == nullptr) {
        return make_ready_json_future(json{{"error", "unknown tool: " + name},
                                            {"code", "unknown_tool"},
                                            {"details", json{{"name", name}}}});
    }
    if (!is_tool_allowed(name)) {
        return make_ready_json_future(json{{"error", "tool not allowed by AGENT_TOOL_ALLOWLIST"},
                                            {"code", "tool_not_allowed"},
                                            {"details", json{{"name", name}}}});
    }
    ToolMeta tm = tool->get_tool_meta(name);
    const auto authorize = [&](const json& candidate) -> std::optional<json> {
        if (!control.authorization) return std::nullopt;
        try {
            return control.authorization(name, candidate, tm);
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
        if (auto hook_err = run_tool_call_hooks(name, current, hooks_copy)) {
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
    return tool->call_cancellable(name, current, control);
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
    auto tool = find_tool(name);
    if (tool == nullptr) {
        return ToolMeta{};
    }
    return tool->get_tool_meta(name);
}

std::optional<ToolInfo> ToolBus::get_tool_info(const std::string& name) const {
    auto tool = find_tool(name);
    if (tool == nullptr) {
        return std::nullopt;
    }
    return tool->get_tool_info(name);
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
                                                                        bool register_all) {
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
    for (auto it = servers.begin(); it != servers.end(); ++it) {
        const std::string service_name = it.key();
        const json& s = it.value();
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
