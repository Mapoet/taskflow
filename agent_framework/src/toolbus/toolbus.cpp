/**
 * @file toolbus.cpp
 * @brief ToolBus implementation (WP1.2)
 */

#include "agent/toolbus.hpp"

#include "agent/mcp_client.hpp"
#include "agent/schema_validate.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <future>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
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
    std::vector<ToolMeta> metas = client->list_tools().get();
    for (const auto& m : metas) {
        const std::string reg = service_name + "__" + m.name;
        if (al.has_value() && al->count(reg) == 0U) {
            throw std::invalid_argument("AGENT_TOOL_ALLOWLIST blocks MCP tool: " + reg);
        }
    }
    std::lock_guard<std::mutex> lock(tools_mutex_);
    if (mcp_services_.count(service_name) != 0U) {
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
    mcp_services_.insert(service_name);
}

void ToolBus::register_api_tool(const std::string& /*name*/, const std::string& /*endpoint*/,
                                const std::string& /*method*/, const ToolMeta& /*meta*/) {
    throw std::logic_error("WP1.3: API tool not implemented (register_api_tool)");
}

std::future<json> ToolBus::call_tool(const std::string& name, const json& arguments) {
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
    json err = json::object();
    ToolMeta tm = tool->get_tool_meta(name);
    const json& schema = tm.schema;
    if (!validate_tool_arguments(schema, arguments, err)) {
        return make_ready_json_future(std::move(err));
    }
    return tool->call(name, arguments);
}

std::vector<ToolMeta> ToolBus::export_as_llm_tools() const {
    std::lock_guard<std::mutex> lock(tools_mutex_);
    std::vector<ToolMeta> out;
    out.reserve(tools_.size());
    for (const auto& kv : tools_) {
        ToolMeta m = kv.second->get_tool_meta(kv.first);
        if (!m.name.empty()) {
            out.push_back(std::move(m));
        }
    }
    return out;
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

    const json& servers = doc["mcpServers"];
    for (auto it = servers.begin(); it != servers.end(); ++it) {
        const std::string service_name = it.key();
        const json& s = it.value();
        if (!s.is_object()) {
            result.failures.push_back(
                CursorMcpImportFailure{service_name, "server entry must be a JSON object"});
            continue;
        }

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
                }
                std::map<std::string, std::string> env;
                if (s.contains("env")) {
                    env = parse_string_map(s["env"]);
                }
                client = MCPClient::create_stdio(command, args, env);
            } else {
                throw std::invalid_argument("unknown server type (need url or command)");
            }

            register_mcp_service(service_name, client);
            result.registered_services.push_back(service_name);
        } catch (const std::exception& e) {
            result.failures.push_back(CursorMcpImportFailure{service_name, e.what()});
        }
    }

    return result;
}

} // namespace agent_framework
