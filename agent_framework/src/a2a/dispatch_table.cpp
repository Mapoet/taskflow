/**
 * @file dispatch_table.cpp
 */
#include <agent/a2a/dispatch_table.hpp>

namespace agent_framework {
namespace a2a {

JsonRpcInvokeError::JsonRpcInvokeError(int c, std::string message, json d)
    : std::runtime_error(std::move(message)), code(c), data(std::move(d)) {}

void DispatchTable::register_method(std::string name, JsonRpcMethodHandler handler) {
    handlers_[std::move(name)] = std::move(handler);
}

json DispatchTable::invoke(const std::string& method, const json& params) const {
    auto it = handlers_.find(method);
    if (it == handlers_.end()) {
        throw JsonRpcInvokeError(-32601, "Method not found");
    }
    try {
        return it->second(params);
    } catch (const JsonRpcInvokeError&) {
        throw;
    } catch (const std::exception& e) {
        throw JsonRpcInvokeError(-32603, "Internal error", json{{"detail", e.what()}});
    } catch (...) {
        throw JsonRpcInvokeError(-32603, "Internal error");
    }
}

bool DispatchTable::has_method(const std::string& method) const {
    return handlers_.find(method) != handlers_.end();
}

void validate_send_message_params_for_dispatch(const json& params) {
    if (!params.contains("message") || !params["message"].is_object()) {
        throw JsonRpcInvokeError(-32602, "Invalid params: missing message object");
    }
    const json& msg = params["message"];
    if (!msg.contains("role")) {
        throw JsonRpcInvokeError(-32602, "Invalid params: message.role");
    }
    if (!msg.contains("parts") || !msg["parts"].is_array()) {
        throw JsonRpcInvokeError(-32602, "Invalid params: message.parts");
    }
}

} // namespace a2a
} // namespace agent_framework
