/**
 * @file mcptool.cpp
 * @brief MCPProxyTool、MCPTool、APITool 实现
 */

#include <agent/toolbus/toolbus.hpp>

#include <agent/toolbus/schema_validate.hpp>

#include <future>
#include <utility>

namespace agent_framework {
namespace {

std::future<json> make_ready_json_future(json j) {
    std::promise<json> p;
    p.set_value(std::move(j));
    return p.get_future();
}

} // namespace

MCPProxyTool::MCPProxyTool(std::shared_ptr<MCPClient> client, std::string registered_name,
                             std::string remote_tool_name, ToolMeta meta)
    : client_(std::move(client)), registered_name_(std::move(registered_name)),
      remote_tool_name_(std::move(remote_tool_name)), meta_(std::move(meta)) {
    info_.name = registered_name_;
}

std::future<json> MCPProxyTool::call(const std::string& name, const json& arguments) {
    return call_cancellable(name, arguments, {});
}

std::future<json> MCPProxyTool::call_cancellable(const std::string& name, const json& arguments,
                                                 const ToolCallControl& control) {
    if (name != registered_name_) {
        return make_ready_json_future(
            json{{"error", "tool name does not match MCP proxy registration"},
                 {"code", "tool_internal_error"},
                 {"details",
                  json{{"reason", "name mismatch"}, {"expected", registered_name_}, {"got", name}}}});
    }
    return client_->call_tool(remote_tool_name_, arguments, control.cancellation_requested);
}

ToolMeta MCPProxyTool::get_tool_meta(const std::string& name) const {
    if (name != registered_name_) {
        return ToolMeta{};
    }
    return meta_;
}

std::vector<std::string> MCPProxyTool::list_tools() const {
    return {registered_name_};
}

bool MCPProxyTool::validate_arguments(const std::string& name, const json& arguments) const {
    if (name != registered_name_) {
        return false;
    }
    if (meta_.schema.is_null() || meta_.schema.empty()) {
        return true;
    }
    json err = json::object();
    return validate_tool_arguments(meta_.schema, arguments, err);
}

std::optional<ToolInfo> MCPProxyTool::get_tool_info(const std::string& name) const {
    if (name != registered_name_) {
        return std::nullopt;
    }
    return info_;
}

MCPTool::MCPTool(std::shared_ptr<MCPClient> client) : client_(std::move(client)) {}

void MCPTool::refresh_tools_cache() {
    if (!client_) {
        return;
    }
    try {
        auto fut = client_->list_tools();
        auto tools = fut.get();
        std::lock_guard<std::mutex> lock(cache_mutex_);
        cached_tools_ = std::move(tools);
    } catch (...) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        cached_tools_.clear();
    }
}

std::future<json> MCPTool::call(const std::string& name, const json& arguments) {
    return call_cancellable(name, arguments, {});
}

std::future<json> MCPTool::call_cancellable(const std::string& name, const json& arguments,
                                            const ToolCallControl& control) {
    return client_ ? client_->call_tool(name, arguments, control.cancellation_requested)
                   : make_ready_json_future(json{{"error", "no MCP client"},
                                                 {"code", "mcp_jsonrpc_error"},
                                                 {"details", json::object()}});
}

ToolMeta MCPTool::get_tool_meta(const std::string& name) const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    for (const auto& t : cached_tools_) {
        if (t.name == name) {
            return t;
        }
    }
    return ToolMeta{};
}

std::vector<std::string> MCPTool::list_tools() const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    std::vector<std::string> n;
    for (const auto& t : cached_tools_) {
        n.push_back(t.name);
    }
    return n;
}

bool MCPTool::validate_arguments(const std::string& name, const json& arguments) const {
    ToolMeta m;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        for (const auto& t : cached_tools_) {
            if (t.name == name) {
                m = t;
                break;
            }
        }
    }
    if (m.name.empty()) {
        return false;
    }
    if (m.schema.is_null() || m.schema.empty()) {
        return true;
    }
    json err = json::object();
    return validate_tool_arguments(m.schema, arguments, err);
}

std::optional<ToolInfo> MCPTool::get_tool_info(const std::string& name) const {
    ToolMeta m = get_tool_meta(name);
    if (m.name.empty()) {
        return std::nullopt;
    }
    ToolInfo i;
    i.name = m.name;
    return i;
}

APITool::APITool(const std::string& name, const std::string& endpoint, const std::string& method,
                 const ToolMeta& meta)
    : name_(name), endpoint_(endpoint), method_(method), meta_(meta) {
    meta_.name = name_;
    info_.name = name_;
}

std::future<json> APITool::call(const std::string& /*name*/, const json& /*arguments*/) {
    std::promise<json> p;
    p.set_value(json{{"error", "not implemented in WP1.2"},
                     {"code", "api_not_implemented"},
                     {"details", json::object()}});
    return p.get_future();
}

ToolMeta APITool::get_tool_meta(const std::string& name) const {
    if (name != name_) {
        return ToolMeta{};
    }
    return meta_;
}

std::vector<std::string> APITool::list_tools() const {
    return {name_};
}

bool APITool::validate_arguments(const std::string& /*name*/, const json& /*arguments*/) const {
    return false;
}

std::optional<ToolInfo> APITool::get_tool_info(const std::string& name) const {
    if (name != name_) {
        return std::nullopt;
    }
    return info_;
}

json APITool::send_http_request(const json& /*payload*/) {
    return json::object();
}

} // namespace agent_framework
