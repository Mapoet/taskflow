/**
 * @file jsonrpc.hpp
 * @brief JSON-RPC 2.0 通用信封（WP2.1a；无 A2A 业务 method）
 * @author Mapoet
 * @version 0.1
 * @date 2026-04-05
 */
#ifndef __AGENT_A2A_JSONRPC_H__
#define __AGENT_A2A_JSONRPC_H__

#include <agent/types.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <variant> // JsonRpcParseResult

namespace agent_framework {
namespace a2a {

/** @brief JSON-RPC 2.0 标准错误码子集（见 a2a-spec-tracker.md §8.4） */
namespace JsonRpcErrorCode {
inline constexpr int parse_error = -32700;
inline constexpr int invalid_request = -32600;
inline constexpr int method_not_found = -32601;
inline constexpr int invalid_params = -32602;
inline constexpr int internal_error = -32603;
} // namespace JsonRpcErrorCode

/**
 * @brief 成功解析后的单请求（非 batch、非 notification）
 */
struct JsonRpcRequest {
    std::string method;
    std::optional<::json> params;
    /** @brief 请求的 id，仅为 JSON number（整数）或 string（与规范一致） */
    ::json id;
};

/**
 * @brief 解析结果：成功为 JsonRpcRequest；失败为完整 error response JSON
 */
using JsonRpcParseResult = std::variant<JsonRpcRequest, ::json>;

/**
 * @brief 解析 JSON-RPC 2.0 单请求体（UTF-8）
 *
 * 策略见 docs/guides/a2a-spec-tracker.md §8.1：不支持 notification、不支持 batch、params 若存在须为 object。
 */
JsonRpcParseResult parse_jsonrpc_request(std::string_view body_utf8);

/** @brief Parse error：`id` 为 null，`code` -32700 */
::json make_parse_error_response(std::string_view message = "Parse error");

/**
 * @brief 成功响应
 * @param id 须与请求 id 类型一致（integer 或 string）
 */
::json make_success_response(const ::json& id, const ::json& result);

/**
 * @brief 错误响应
 * @param id 与请求相同；无法获知时传 `json()`（JSON null）
 */
::json make_error_response(const ::json& id,
                           int code,
                           std::string_view message,
                           const ::json& data = ::json());

/** @brief 从响应 JSON 读取 error.code；非 error 响应返回 nullopt */
std::optional<int> try_get_jsonrpc_error_code(const ::json& response);

/** @brief 从成功响应读取 result；非成功返回 nullopt */
std::optional<::json> try_get_jsonrpc_result(const ::json& response);

/** @brief 读取响应 id（成功或失败均可） */
::json try_get_jsonrpc_response_id(const ::json& response);

} // namespace a2a
} // namespace agent_framework

#endif // __AGENT_A2A_JSONRPC_H__
