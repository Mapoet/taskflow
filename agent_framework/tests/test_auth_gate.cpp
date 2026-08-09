/**
 * @file test_auth_gate.cpp
 * @brief Unit tests for AuthGate / parse_auth_requirement (WP2.5)
 */
#include <agent/a2a/auth_gate.hpp>
#include <agent/a2a/auth_requirement.hpp>

#include <httplib.hpp>

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <cstring>

using namespace agent_framework;
using namespace agent_framework::a2a;

namespace {
class FakeClaimsValidator final : public BearerClaimsValidator {
public:
    BearerValidationResult validate(std::string_view token) const override {
        if (token != "signed-token") return {false, {}, "invalid_signature"};
        BearerClaims claims;
        claims.subject = "subject";
        claims.issuer = "https://issuer.example";
        claims.audiences = {"taskflow-agent"};
        claims.scopes = {"tasks.read", "tasks.stream"};
        return {true, std::move(claims), ""};
    }
};
}

static void fail(const char* m) {
    std::cerr << "test_auth_gate: " << m << "\n";
    std::exit(1);
}

static void test_constant_time_equal() {
    assert(!constant_time_equal("a", "ab"));
    assert(constant_time_equal("secret", "secret"));
    assert(!constant_time_equal("secret", "secreT"));
}

static void test_parse_bearer_scheme() {
    json sch = json::object();
    sch["mybearer"] = json{{"type", "http"}, {"scheme", "bearER"}};
    AuthRequirement r;
    std::string w;
    assert(parse_auth_requirement(sch, false, &w, &r));
    assert(r.kind == AuthRequirementKind::HttpBearer);
}

static void test_parse_api_key_header() {
    json sch = json::object();
    sch["k"] = json{{"type", "apiKey"}, {"in", "header"}, {"name", "X-Custom"}};
    AuthRequirement r;
    assert(parse_auth_requirement(sch, false, nullptr, &r));
    assert(r.kind == AuthRequirementKind::ApiKeyHeader);
    assert(r.api_key_header_name == "X-Custom");
}

static void test_gate_bearer_ok() {
    AuthGateConfig cfg;
    cfg.mode = ServerAuthMode::Bearer;
    cfg.effective.kind = AuthRequirementKind::HttpBearer;
    cfg.bearer_tokens = {"tok1", "tok2"};

    AuthContext ctx;
    ctx.headers_lower["authorization"] = "Bearer tok2";

    AuthFailure f;
    assert(auth_gate_check(ctx, cfg, &f));
}

static void test_gate_bearer_missing_www() {
    AuthGateConfig cfg;
    cfg.mode = ServerAuthMode::Bearer;
    cfg.effective.kind = AuthRequirementKind::HttpBearer;
    cfg.bearer_tokens = {"t"};

    AuthContext ctx;
    AuthFailure f;
    assert(!auth_gate_check(ctx, cfg, &f));
    assert(!f.www_authenticate.empty());
}

static void test_gate_api_query() {
    AuthGateConfig cfg;
    cfg.mode = ServerAuthMode::ApiKeyQuery;
    cfg.effective.kind = AuthRequirementKind::ApiKeyQuery;
    cfg.effective.api_key_query_param = "api_key";
    cfg.api_keys = {"secret"};

    httplib::Request req;
    req.params.insert({"api_key", "secret"});
    AuthContext ctx = auth_context_from_request(req);
    AuthFailure f;
    assert(auth_gate_check(ctx, cfg, &f));
}

static void test_load_config_match_card_bearer() {
#if defined(_WIN32)
    (void)_putenv_s("AGENT_SERVER_AUTH_MODE", "match_card");
    (void)_putenv_s("AGENT_SERVER_BEARER_TOKEN", "abc");
#else
    (void)::setenv("AGENT_SERVER_AUTH_MODE", "match_card", 1);
    (void)::setenv("AGENT_SERVER_BEARER_TOKEN", "abc", 1);
#endif
    AgentCard card;
    card.authentication_scheme = json::object({{"b", json{{"type", "http"}, {"scheme", "bearer"}}}});
    AuthGateConfig cfg;
    std::string err;
    assert(load_auth_gate_config_from_env(card, &cfg, &err));
    assert(cfg.effective.kind == AuthRequirementKind::HttpBearer);
    assert(cfg.bearer_tokens.size() >= 1u);
#if defined(_WIN32)
    (void)_putenv_s("AGENT_SERVER_AUTH_MODE", "");
    (void)_putenv_s("AGENT_SERVER_BEARER_TOKEN", "");
#else
    (void)::unsetenv("AGENT_SERVER_AUTH_MODE");
    (void)::unsetenv("AGENT_SERVER_BEARER_TOKEN");
#endif
}

static void test_claims_and_route_policy() {
    AuthGateConfig cfg;
    cfg.mode = ServerAuthMode::Bearer;
    cfg.effective.kind = AuthRequirementKind::HttpBearer;
    cfg.bearer_claims_validator = std::make_shared<FakeClaimsValidator>();
    cfg.route_policies[AuthRoute::JsonRpc] = {
        false, "https://issuer.example", "taskflow-agent", {"tasks.read"}};
    cfg.route_policies[AuthRoute::Sse] = {
        false, "https://issuer.example", "taskflow-agent", {"tasks.stream"}};
    cfg.route_policies[AuthRoute::WellKnown].allow_anonymous = true;

    AuthContext ctx;
    ctx.headers_lower["authorization"] = "Bearer signed-token";
    AuthFailure failure;
    assert(auth_gate_check_for_route(ctx, cfg, AuthRoute::JsonRpc, &failure));
    assert(auth_gate_check_for_route(ctx, cfg, AuthRoute::Sse, &failure));
    assert(auth_gate_check_for_route({}, cfg, AuthRoute::WellKnown, &failure));

    cfg.route_policies[AuthRoute::JsonRpc].required_scopes.insert("tasks.write");
    assert(!auth_gate_check_for_route(ctx, cfg, AuthRoute::JsonRpc, &failure));
    assert(failure.http_status == 403);
    assert(failure.log_safe_reason == "insufficient_scope");
    assert(failure.www_authenticate.find("insufficient_scope") != std::string::npos);

    ctx.headers_lower["authorization"] = "Bearer tampered-token";
    assert(!auth_gate_check_for_route(ctx, cfg, AuthRoute::Sse, &failure));
    assert(failure.log_safe_reason == "invalid_signature");
    assert(failure.log_safe_reason.find("tampered-token") == std::string::npos);
}

int main() {
    test_constant_time_equal();
    test_parse_bearer_scheme();
    test_parse_api_key_header();
    test_gate_bearer_ok();
    test_gate_bearer_missing_www();
    test_gate_api_query();
    test_load_config_match_card_bearer();
    test_claims_and_route_policy();
    std::cout << "test_auth_gate: ok\n";
    return 0;
}
