/**
 * @file test_verifier_types.cpp
 * @brief WP2.8 Verifier types / parse / gate (phase-2-wp8.md §10.1)
 */
#include <agent/verifier_types.hpp>

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void test_v1_pass() {
    const char* raw = R"({"ok":true,"issues":[],"suggested_action":"pass"})";
    auto r = agent_framework::parse_verifier_response(raw, 0, 1);
    assert(r.ok);
    assert(r.issues.empty());
    assert(r.suggested_action == "pass");
    auto g = agent_framework::apply_verifier_gate(r, 0, 1);
    assert(g.kind == agent_framework::VerifierGateKind::PublishPass);
    assert(g.verifier_ok);
    assert(g.effective_action == "pass");
}

void test_v2_retry_eligible() {
    const char* raw = R"({"ok":false,"issues":[{"code":"x","severity":"warn","detail":"d"}],"suggested_action":"retry_main"})";
    auto r = agent_framework::parse_verifier_response(raw, 0, 1);
    assert(!r.ok);
    auto g = agent_framework::apply_verifier_gate(r, 0, 1);
    assert(g.kind == agent_framework::VerifierGateKind::RetryMain);
    assert(!g.verifier_ok);
    assert(g.effective_action == "retry_main");
}

void test_v3_retry_exhausted() {
    const char* raw = R"({"ok":false,"issues":[{"code":"x","severity":"block","detail":"d"}],"suggested_action":"retry_main"})";
    auto r = agent_framework::parse_verifier_response(raw, 0, 1);
    auto g = agent_framework::apply_verifier_gate(r, 1, 1);
    assert(g.kind == agent_framework::VerifierGateKind::PublishPassThrough);
    assert(!g.verifier_ok);
    assert(g.effective_action == "pass_through");
}

void test_v4_abort() {
    const char* raw = R"({"ok":false,"issues":[{"code":"bad","severity":"block","detail":"no"}],"suggested_action":"abort"})";
    auto r = agent_framework::parse_verifier_response(raw, 0, 1);
    auto g = agent_framework::apply_verifier_gate(r, 0, 1);
    assert(g.kind == agent_framework::VerifierGateKind::Abort);
    assert(g.effective_action == "abort");
}

void test_v5_parse_error_pass_through() {
    auto r = agent_framework::parse_verifier_response("not json {", 0, 1);
    assert(!r.ok);
    assert(r.suggested_action == "pass_through");
    assert(!r.issues.empty());
    assert(r.issues[0].code == "verifier_parse_error");
    auto g = agent_framework::apply_verifier_gate(r, 0, 1);
    assert(g.kind == agent_framework::VerifierGateKind::PublishPassThrough);
}

void test_v6_abort_wins_over_ok() {
    const char* raw = R"({"ok":true,"issues":[],"suggested_action":"abort"})";
    auto r = agent_framework::parse_verifier_response(raw, 0, 1);
    assert(r.ok);
    auto g = agent_framework::apply_verifier_gate(r, 0, 1);
    assert(g.kind == agent_framework::VerifierGateKind::Abort);
    assert(g.effective_action == "abort");
}

void test_default_suggested_action() {
    const char* raw = R"({"ok":true})";
    auto r = agent_framework::parse_verifier_response(raw, 0, 1);
    assert(r.suggested_action == "pass");
    const char* raw2 = R"({"ok":false})";
    auto r2 = agent_framework::parse_verifier_response(raw2, 0, 1);
    assert(r2.suggested_action == "retry_main");
    auto r3 = agent_framework::parse_verifier_response(raw2, 1, 1);
    assert(r3.suggested_action == "pass_through");
}

void test_unknown_action_normalized() {
    const char* raw =
        R"({"ok":false,"issues":[],"suggested_action":"completely_unknown"})";
    auto r = agent_framework::parse_verifier_response(raw, 0, 1);
    auto g = agent_framework::apply_verifier_gate(r, 0, 1);
    assert(g.action_unknown_normalized);
    assert(g.kind == agent_framework::VerifierGateKind::PublishPassThrough);
}

void test_fix_message_cap() {
    agent_framework::VerifierIssue i;
    i.code = "c";
    i.severity = "block";
    i.detail = std::string(600, 'x');
    std::string msg = agent_framework::build_verifier_fix_system_message({i});
    assert(msg.size() <= 2048);
}

} // namespace

int main() {
    test_v1_pass();
    test_v2_retry_eligible();
    test_v3_retry_exhausted();
    test_v4_abort();
    test_v5_parse_error_pass_through();
    test_v6_abort_wins_over_ok();
    test_default_suggested_action();
    test_unknown_action_normalized();
    test_fix_message_cap();
    std::cout << "test_verifier_types: all passed\n";
    return EXIT_SUCCESS;
}
