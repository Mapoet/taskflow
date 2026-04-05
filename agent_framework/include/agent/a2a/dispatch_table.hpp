/**
 * @file dispatch_table.hpp
 * @brief JSON-RPC method 分发（WP2.1；供 WP2.2 挂路由）
 * @author Mapoet
 * @version 0.1
 * @date 2026-04-05
 */
#ifndef __AGENT_A2A_DISPATCH_TABLE_H__
#define __AGENT_A2A_DISPATCH_TABLE_H__

#include <agent/types.hpp>

#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace agent_framework {
namespace a2a {

/** @brief 分发层错误（映射为 JSON-RPC error，见 a2a-spec-tracker.md §8.3） */
class JsonRpcInvokeError : public std::runtime_error {
public:
    int code;
    json data;

    JsonRpcInvokeError(int c, std::string message, json d = json::object());
};

using JsonRpcMethodHandler = std::function<json(const json& params)>;

/**
 * @brief `method` 字符串 → handler；`invoke` 未找到方法抛 JsonRpcInvokeError(-32601)
 */
class DispatchTable {
public:
    void register_method(std::string name, JsonRpcMethodHandler handler);

    /** @throws JsonRpcInvokeError */
    json invoke(const std::string& method, const json& params) const;

    bool has_method(const std::string& method) const;

private:
    std::unordered_map<std::string, JsonRpcMethodHandler> handlers_;
};

/**
 * @brief SendMessage 的 params 最小校验：`message` 对象且含 `role`、`parts` 数组
 * @throws JsonRpcInvokeError -32602
 */
void validate_send_message_params_for_dispatch(const json& params);

} // namespace a2a
} // namespace agent_framework

#endif // __AGENT_A2A_DISPATCH_TABLE_H__
