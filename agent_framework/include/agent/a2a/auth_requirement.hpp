/**
 * @file auth_requirement.hpp
 * @brief Agent Card `securitySchemes` → AuthRequirement (WP2.5)
 * @author Mapoet
 * @version 0.1
 * @date 2026-04-06
 */
#ifndef __AGENT_A2A_AUTH_REQUIREMENT_H__
#define __AGENT_A2A_AUTH_REQUIREMENT_H__

#include <agent/a2a/types.hpp>
#include <agent/core/types.hpp>

#include <optional>
#include <string>

namespace agent_framework {
namespace a2a {

// --- Requirement from card (wire) ---

struct AuthRequirement {
    AuthRequirementKind kind{AuthRequirementKind::None};
    /** Realm hint for WWW-Authenticate (optional). */
    std::optional<std::string> bearer_realm;
    /** API key header name (when kind is ApiKeyHeader). */
    std::string api_key_header_name{"X-API-Key"};
    /** Query parameter name (when kind is ApiKeyQuery). */
    std::string api_key_query_param{"api_key"};
};

/**
 * @brief Map `securitySchemes` JSON (`AgentCard::authentication_scheme`) to AuthRequirement.
 * @param security_schemes_wire Object from agent card wire (`securitySchemes`).
 * @param strict If true, unknown schemes / Multiple / ambiguity → returns false.
 * @param warn_out If non-null and non-strict, append warning lines.
 * @param out Filled on success (even if kind is None).
 * @return false if strict validation failed (out may be None).
 */
bool parse_auth_requirement(const json& security_schemes_wire,
                            bool strict,
                            std::string* warn_out,
                            AuthRequirement* out);

} // namespace a2a
} // namespace agent_framework

#endif // __AGENT_A2A_AUTH_REQUIREMENT_H__
