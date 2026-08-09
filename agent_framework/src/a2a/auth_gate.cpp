/**
 * @file auth_gate.cpp
 * @brief WP2.5 AuthGate implementation
 */
#include <agent/a2a/auth_gate.hpp>

#include <httplib.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace agent_framework {
namespace a2a {

namespace {

std::string to_lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string trim_sv(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    std::size_t j = s.size();
    while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1]))) {
        --j;
    }
    return std::string(s.substr(i, j - i));
}

void split_csv(const std::string& in, std::vector<std::string>* out) {
    out->clear();
    std::size_t start = 0;
    while (start <= in.size()) {
        std::size_t comma = in.find(',', start);
        const std::string part =
            trim_sv(std::string_view(in.data() + start, (comma == std::string::npos ? in.size() : comma) - start));
        if (!part.empty()) {
            out->push_back(part);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
}

bool env_truthy(const char* v) {
    if (!v || !v[0]) {
        return false;
    }
    return v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y';
}

ServerAuthMode parse_mode(const char* m) {
    if (!m || !m[0]) {
        return ServerAuthMode::Off;
    }
    std::string s = to_lower(std::string(m));
    if (s == "off" || s == "none") {
        return ServerAuthMode::Off;
    }
    if (s == "bearer") {
        return ServerAuthMode::Bearer;
    }
    if (s == "api_key_header" || s == "apikey_header") {
        return ServerAuthMode::ApiKeyHeader;
    }
    if (s == "api_key_query" || s == "apikey_query") {
        return ServerAuthMode::ApiKeyQuery;
    }
    if (s == "match_card") {
        return ServerAuthMode::MatchCard;
    }
    return ServerAuthMode::Off;
}

std::string log_safe_token_hint(std::string_view tok) {
    if (tok.empty()) {
        return "len=0";
    }
    if (tok.size() <= 4) {
        return "len=" + std::to_string(tok.size());
    }
    return std::string("…") + std::string(tok.substr(tok.size() - 4));
}

} // namespace

bool constant_time_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char acc = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        acc |= static_cast<unsigned char>(
            static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]));
    }
    return acc == 0;
}

AuthContext auth_context_from_request(const httplib::Request& req) {
    AuthContext ctx;
    for (const auto& h : req.headers) {
        std::string key = to_lower(h.first);
        ctx.headers_lower[std::move(key)] = h.second;
    }
    for (const auto& p : req.params) {
        std::string key = to_lower(p.first);
        if (ctx.params_lower.find(key) == ctx.params_lower.end()) {
            ctx.params_lower[std::move(key)] = p.second;
        }
    }
    return ctx;
}

bool load_auth_gate_config_from_env(const AgentCard& card, AuthGateConfig* cfg, std::string* err_out) {
    if (!cfg) {
        if (err_out) {
            *err_out = "cfg null";
        }
        return false;
    }
    *cfg = AuthGateConfig{};
    cfg->route_policies.emplace(AuthRoute::WellKnown, RouteAuthPolicy{});
    cfg->route_policies.emplace(AuthRoute::JsonRpc, RouteAuthPolicy{});
    cfg->route_policies.emplace(AuthRoute::Sse, RouteAuthPolicy{});
    cfg->route_policies.emplace(AuthRoute::LegacyRest, RouteAuthPolicy{});
    const char* mode_e = std::getenv("AGENT_SERVER_AUTH_MODE");
    cfg->mode = parse_mode(mode_e);

    if (const char* h = std::getenv("AGENT_SERVER_API_KEY_HEADER")) {
        if (h[0]) {
            cfg->env_api_key_header_name = h;
        }
    }
    if (const char* q = std::getenv("AGENT_SERVER_API_KEY_QUERY")) {
        if (q[0]) {
            cfg->env_api_key_query_param = q;
        }
    }

    std::string warns;
    const bool strict = env_truthy(std::getenv("AGENT_SERVER_STRICT_CARD_AUTH"));

    if (cfg->mode == ServerAuthMode::MatchCard) {
        if (!parse_auth_requirement(card.authentication_scheme, strict, &warns, &cfg->effective)) {
            if (err_out) {
                *err_out = "strict auth: failed to parse card securitySchemes";
            }
            return false;
        }
        if (cfg->effective.kind == AuthRequirementKind::ApiKeyHeader) {
            (void)0;
        } else if (cfg->effective.kind == AuthRequirementKind::ApiKeyQuery) {
            (void)0;
        }
    } else if (cfg->mode == ServerAuthMode::Bearer) {
        cfg->effective.kind = AuthRequirementKind::HttpBearer;
    } else if (cfg->mode == ServerAuthMode::ApiKeyHeader) {
        cfg->effective.kind = AuthRequirementKind::ApiKeyHeader;
        cfg->effective.api_key_header_name = cfg->env_api_key_header_name;
    } else if (cfg->mode == ServerAuthMode::ApiKeyQuery) {
        cfg->effective.kind = AuthRequirementKind::ApiKeyQuery;
        cfg->effective.api_key_query_param = cfg->env_api_key_query_param;
    } else {
        cfg->effective.kind = AuthRequirementKind::None;
    }

    cfg->bearer_tokens.clear();
    if (const char* t = std::getenv("AGENT_SERVER_BEARER_TOKEN")) {
        if (t[0]) {
            cfg->bearer_tokens.push_back(std::string(t));
        }
    }
    if (const char* ts = std::getenv("AGENT_SERVER_BEARER_TOKENS")) {
        if (ts[0]) {
            std::vector<std::string> extra;
            split_csv(std::string(ts), &extra);
            cfg->bearer_tokens.insert(cfg->bearer_tokens.end(), extra.begin(), extra.end());
        }
    }

    if (const char* ks = std::getenv("AGENT_SERVER_API_KEYS")) {
        if (ks[0]) {
            split_csv(std::string(ks), &cfg->api_keys);
        }
    }

    const bool need_bearer = (cfg->mode != ServerAuthMode::Off &&
                              cfg->effective.kind == AuthRequirementKind::HttpBearer);
    const bool need_key = (cfg->mode != ServerAuthMode::Off &&
                           (cfg->effective.kind == AuthRequirementKind::ApiKeyHeader ||
                            cfg->effective.kind == AuthRequirementKind::ApiKeyQuery));

    if (need_bearer && cfg->bearer_tokens.empty()) {
        if (err_out) {
            *err_out = "AGENT_SERVER_AUTH_MODE requires bearer but no AGENT_SERVER_BEARER_TOKEN(S)";
        }
        return false;
    }
    if (need_key && cfg->api_keys.empty()) {
        if (err_out) {
            *err_out = "AGENT_SERVER_AUTH_MODE requires api key but AGENT_SERVER_API_KEYS empty";
        }
        return false;
    }

    if (cfg->mode == ServerAuthMode::MatchCard && cfg->effective.kind == AuthRequirementKind::HttpBearer &&
        cfg->bearer_tokens.empty()) {
        if (err_out) {
            *err_out = "match_card bearer requires AGENT_SERVER_BEARER_TOKEN(S)";
        }
        return false;
    }
    if (cfg->mode == ServerAuthMode::MatchCard &&
        (cfg->effective.kind == AuthRequirementKind::ApiKeyHeader ||
         cfg->effective.kind == AuthRequirementKind::ApiKeyQuery) &&
        cfg->api_keys.empty()) {
        if (err_out) {
            *err_out = "match_card api key requires AGENT_SERVER_API_KEYS";
        }
        return false;
    }

    (void)warns;
    return true;
}

bool auth_gate_check(const AuthContext& ctx, const AuthGateConfig& cfg, AuthFailure* fail_out) {
    return auth_gate_check_for_route(ctx, cfg, AuthRoute::JsonRpc, fail_out);
}

bool auth_gate_check_for_route(const AuthContext& ctx, const AuthGateConfig& cfg,
                               AuthRoute route, AuthFailure* fail_out) {
    const auto policy_it = cfg.route_policies.find(route);
    const RouteAuthPolicy policy = policy_it == cfg.route_policies.end()
        ? RouteAuthPolicy{} : policy_it->second;
    if (policy.allow_anonymous) return true;
    if (cfg.mode == ServerAuthMode::Off) {
        return true;
    }
    if (cfg.effective.kind == AuthRequirementKind::None) {
        return true;
    }
    AuthFailure fail;
    fail.http_status = 401;
    fail.body_json = R"({"error":"Unauthorized"})";

    if (cfg.effective.kind == AuthRequirementKind::HttpBearer) {
        auto it = ctx.headers_lower.find("authorization");
        if (it == ctx.headers_lower.end()) {
            fail.www_authenticate = "Bearer";
            fail.log_safe_reason = "missing Authorization";
            if (fail_out) {
                *fail_out = std::move(fail);
            }
            return false;
        }
        std::string_view val = it->second;
        const std::string prefix = trim_sv(val);
        std::size_t sp = prefix.find(' ');
        if (sp == std::string::npos) {
            fail.www_authenticate = "Bearer";
            fail.log_safe_reason = "malformed Authorization";
            if (fail_out) {
                *fail_out = std::move(fail);
            }
            return false;
        }
        std::string scheme = to_lower(prefix.substr(0, sp));
        std::string token = trim_sv(std::string_view(prefix.c_str() + sp + 1, prefix.size() - sp - 1));
        if (scheme != "bearer") {
            fail.www_authenticate = "Bearer";
            fail.log_safe_reason = "authorization scheme not bearer";
            if (fail_out) {
                *fail_out = std::move(fail);
            }
            return false;
        }
        if (cfg.bearer_claims_validator) {
            const auto validation = cfg.bearer_claims_validator->validate(token);
            if (!validation.valid) {
                fail.www_authenticate = "Bearer error=\"invalid_token\"";
                fail.log_safe_reason = validation.error_code.empty() ? "invalid_token" : validation.error_code;
                if (fail_out) *fail_out = std::move(fail);
                return false;
            }
            const bool issuer_ok = policy.required_issuer.empty() ||
                validation.claims.issuer == policy.required_issuer;
            const bool audience_ok = policy.required_audience.empty() ||
                validation.claims.audiences.contains(policy.required_audience);
            const bool scopes_ok = std::all_of(policy.required_scopes.begin(), policy.required_scopes.end(),
                [&](const std::string& scope) { return validation.claims.scopes.contains(scope); });
            if (!issuer_ok || !audience_ok || !scopes_ok) {
                fail.http_status = scopes_ok ? 401 : 403;
                fail.www_authenticate = scopes_ok ? "Bearer error=\"invalid_token\""
                                                  : "Bearer error=\"insufficient_scope\"";
                fail.body_json = scopes_ok ? R"({"error":"Unauthorized"})"
                                           : R"({"error":"Forbidden"})";
                fail.log_safe_reason = !issuer_ok ? "issuer_mismatch" :
                    !audience_ok ? "audience_mismatch" : "insufficient_scope";
                if (fail_out) *fail_out = std::move(fail);
                return false;
            }
            return true;
        }
        bool ok = false;
        for (const auto& t : cfg.bearer_tokens)
            if (constant_time_equal(token, t)) { ok = true; break; }
        if (!ok) {
            fail.www_authenticate = "Bearer";
            fail.log_safe_reason = "bearer mismatch " + log_safe_token_hint(token);
            if (fail_out) {
                *fail_out = std::move(fail);
            }
            return false;
        }
        return true;
    }

    if (cfg.effective.kind == AuthRequirementKind::ApiKeyHeader) {
        const std::string hk = to_lower(cfg.effective.api_key_header_name);
        auto it = ctx.headers_lower.find(hk);
        if (it == ctx.headers_lower.end()) {
            fail.log_safe_reason = "missing api key header";
            if (fail_out) {
                *fail_out = std::move(fail);
            }
            return false;
        }
        bool ok = false;
        for (const auto& k : cfg.api_keys) {
            if (constant_time_equal(it->second, k)) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            fail.log_safe_reason = "api key header mismatch";
            if (fail_out) {
                *fail_out = std::move(fail);
            }
            return false;
        }
        return true;
    }

    if (cfg.effective.kind == AuthRequirementKind::ApiKeyQuery) {
        const std::string pk = to_lower(cfg.effective.api_key_query_param);
        auto it = ctx.params_lower.find(pk);
        if (it == ctx.params_lower.end()) {
            fail.log_safe_reason = "missing api key query";
            if (fail_out) {
                *fail_out = std::move(fail);
            }
            return false;
        }
        bool ok = false;
        for (const auto& k : cfg.api_keys) {
            if (constant_time_equal(it->second, k)) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            fail.log_safe_reason = "api key query mismatch";
            if (fail_out) {
                *fail_out = std::move(fail);
            }
            return false;
        }
        return true;
    }

    return true;
}

} // namespace a2a
} // namespace agent_framework
