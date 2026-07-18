/**
 * @file local_tool.cpp
 * @brief Local tool implementation (WP1.2)
 */

#include <agent/toolbus/toolbus.hpp>

#include <agent/context_budget/context_budget.hpp>
#include <agent/toolbus/schema_validate.hpp>

#include <future>
#include <string>

namespace agent_framework {
namespace {

constexpr std::size_t k_exception_what_max = 512;

std::string truncate_what(const char* s) {
    if (s == nullptr) {
        return {};
    }
    return utf8_safe_truncate(s, k_exception_what_max);
}

std::future<json> make_ready_json_future(json j) {
    std::promise<json> p;
    p.set_value(std::move(j));
    return p.get_future();
}

json tool_exception_payload(const std::string& what) {
    return json{{"error", "tool execution failed"},
                {"code", "tool_exception"},
                {"details", json{{"what", what}}}};
}

} // namespace

LocalTool::LocalTool(const std::string& name, std::function<json(const json&)> func,
                     const ToolMeta& meta)
    : name_(name), func_(std::move(func)), meta_(meta) {
    meta_.name = name_;
    info_.name = name_;
}

LocalTool::LocalTool(const std::string& name,
                     std::function<json(const json&, const ToolCallControl&)> func,
                     const ToolMeta& meta)
    : name_(name), cancellable_func_(std::move(func)), meta_(meta) {
    meta_.name = name_;
    info_.name = name_;
}

std::future<json> LocalTool::call(const std::string& name, const json& arguments) {
    return call_cancellable(name, arguments, {});
}

std::future<json> LocalTool::call_cancellable(const std::string& name, const json& arguments,
                                              const ToolCallControl& control) {
    if (name != name_) {
        return make_ready_json_future(
            json{{"error", "tool name does not match LocalTool registration"},
                 {"code", "tool_internal_error"},
                 {"details",
                  json{{"reason", "name mismatch"}, {"expected", name_}, {"got", name}}}});
    }
    return std::async(std::launch::async, [this, arguments, control]() {
        try {
            if (control.should_stop()) {
                return json{{"error", "tool call cancelled"}, {"code", "cancelled"}};
            }
            return cancellable_func_ ? cancellable_func_(arguments, control) : func_(arguments);
        } catch (const std::exception& e) {
            return tool_exception_payload(truncate_what(e.what()));
        } catch (...) {
            return tool_exception_payload("non-std exception");
        }
    });
}

ToolMeta LocalTool::get_tool_meta(const std::string& name) const {
    if (name != name_) {
        return ToolMeta{};
    }
    return meta_;
}

std::vector<std::string> LocalTool::list_tools() const {
    return {name_};
}

bool LocalTool::validate_arguments(const std::string& name, const json& arguments) const {
    if (name != name_) {
        return false;
    }
    json err = json::object();
    return validate_tool_arguments(meta_.schema, arguments, err);
}

std::optional<ToolInfo> LocalTool::get_tool_info(const std::string& name) const {
    if (name != name_) {
        return std::nullopt;
    }
    return info_;
}

} // namespace agent_framework
