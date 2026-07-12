/**
 * @file client_config.hpp
 * @brief A2A client literals aligned with a2a-spec-tracker.md §3 / §7 (WP2.4)
 * @author Mapoet
 * @version 0.1
 * @date 2026-04-05
 */
#ifndef __AGENT_A2A_CLIENT_CONFIG_H__
#define __AGENT_A2A_CLIENT_CONFIG_H__

namespace agent_framework {
namespace a2a {

/** @brief Default JSON-RPC POST path when AGENT_CLIENT_JSON_RPC_PATH is unset (matches Server default). */
inline constexpr const char* kA2aJsonRpcDefaultPath = "/";

/** @brief JSON-RPC method names (tracker §3, PascalCase). */
inline constexpr const char* kMethodSendMessage = "SendMessage";
inline constexpr const char* kMethodGetTask = "GetTask";
inline constexpr const char* kMethodCancelTask = "CancelTask";
inline constexpr const char* kMethodSendStreamingMessage = "SendStreamingMessage";
inline constexpr const char* kMethodSubscribeToTask = "SubscribeToTask";

/**
 * @brief HTTP SSE subscribe path prefix (tracker §7 legacy对照).
 * Full URL: join_url(server_url, kTaskSseSubscribePathQueryPrefix + task_id).
 */
inline constexpr const char* kTaskSseSubscribePathQueryPrefix = "/tasks/sendSubscribe?task_id=";

} // namespace a2a
} // namespace agent_framework

#endif // __AGENT_A2A_CLIENT_CONFIG_H__
