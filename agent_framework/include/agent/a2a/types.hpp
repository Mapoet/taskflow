/**
 * @file types.hpp
 * @brief A2A authentication enumerations (WP2.5)
 * @author Mapoet
 * @version 0.1
 * @date 2026-04-06
 */
#ifndef __AGENT_A2A_TYPES_H__
#define __AGENT_A2A_TYPES_H__

namespace agent_framework {
namespace a2a {

/** @brief Parsed requirement kind from wire `securitySchemes` (OpenAPI subset). */
enum class AuthRequirementKind {
    None,
    HttpBearer,
    ApiKeyHeader,
    ApiKeyQuery,
    Multiple
};

/** @brief Server-side auth mode from environment (`AGENT_SERVER_AUTH_MODE`). */
enum class ServerAuthMode {
    Off,
    Bearer,
    ApiKeyHeader,
    ApiKeyQuery,
    MatchCard
};

} // namespace a2a
} // namespace agent_framework

#endif // __AGENT_A2A_TYPES_H__
