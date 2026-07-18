/**
 * @file mcp_protocol.hpp
 * @brief MCP JSON-RPC 方法名等常量（须与 docs/guides/mcp-spec-tracker.md 一致）
 */
#ifndef __AGENT_MCP_PROTOCOL_H__
#define __AGENT_MCP_PROTOCOL_H__

#include <string_view>

namespace agent_framework {
namespace mcp_protocol {

inline constexpr std::string_view k_jsonrpc_version = "2.0";
inline constexpr std::string_view k_protocol_version = "2024-11-05";

inline constexpr std::string_view k_method_initialize = "initialize";
inline constexpr std::string_view k_method_notifications_initialized = "notifications/initialized";
inline constexpr std::string_view k_method_tools_list = "tools/list";
inline constexpr std::string_view k_method_tools_call = "tools/call";
inline constexpr std::string_view k_method_resources_list = "resources/list";
inline constexpr std::string_view k_method_resources_read = "resources/read";

} // namespace mcp_protocol
} // namespace agent_framework

#endif // __AGENT_MCP_PROTOCOL_H__
