/**
 * @file auth_gate.hpp
 * @brief WP2.5 HTTP auth gate: AuthContext, config from env/card, builtin check
 * @author Mapoet
 * @version 0.1
 * @date 2026-04-06
 */
#ifndef __AGENT_A2A_AUTH_GATE_H__
#define __AGENT_A2A_AUTH_GATE_H__

#include <agent/a2a/auth_requirement.hpp>

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace httplib {
struct Request;
}

namespace agent_framework {
namespace a2a {

// --- Request context (normalized headers & query) ---

/** @brief Lowercased header keys / param keys for uniform lookup. */
struct AuthContext {
    std::map<std::string, std::string> headers_lower;
    std::map<std::string, std::string> params_lower;
};

/** @brief Build AuthContext from httplib request (headers + query params). */
AuthContext auth_context_from_request(const httplib::Request& req);

// --- Gate configuration & failure response ---

struct AuthGateConfig {
    ServerAuthMode mode{ServerAuthMode::Off};
    /** After resolve: what to enforce (None = allow anonymous). */
    AuthRequirement effective;
    std::vector<std::string> bearer_tokens;
    std::vector<std::string> api_keys;
    /** Env defaults when mode is api_key_* (also used as fallback names for match_card). */
    std::string env_api_key_header_name{"X-API-Key"};
    std::string env_api_key_query_param{"api_key"};
};

struct AuthFailure {
    int http_status{401};
    std::string www_authenticate;
    std::string body_json{R"({"error":"Unauthorized"})"};
    std::string log_safe_reason;
};

/**
 * @brief Load AuthGateConfig from environment + optional card (`match_card`).
 * @param card Used when mode is MatchCard.
 * @param err_out Human-readable startup error (no secrets).
 * @return false if configuration is invalid (caller should refuse start).
 */
bool load_auth_gate_config_from_env(const AgentCard& card, AuthGateConfig* cfg, std::string* err_out);

// --- Authorization decision ---

/** @brief Builtin gate: returns true if authorized. On false, fills failure. */
bool auth_gate_check(const AuthContext& ctx, const AuthGateConfig& cfg, AuthFailure* fail_out);

/** @brief Constant-time equality for secret strings (same length only). */
bool constant_time_equal(std::string_view a, std::string_view b);

} // namespace a2a
} // namespace agent_framework

#endif // __AGENT_A2A_AUTH_GATE_H__
