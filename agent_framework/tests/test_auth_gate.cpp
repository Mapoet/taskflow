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

int main() {
    test_constant_time_equal();
    test_parse_bearer_scheme();
    test_parse_api_key_header();
    test_gate_bearer_ok();
    test_gate_bearer_missing_www();
    test_gate_api_query();
    test_load_config_match_card_bearer();
    std::cout << "test_auth_gate: ok\n";
    return 0;
}
