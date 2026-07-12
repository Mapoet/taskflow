/**
 * @file test_a2a_contract_json.cpp
 * @brief WP2.6 Tier A: drive synthetic-v1 manifest (JSON-RPC, Card, Task wire)
 */
#include <agent/a2a/jsonrpc.hpp>
#include <agent/a2a/wire_card.hpp>
#include <agent/a2a/wire_mapping.hpp>

#include "a2a_contract_helpers.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

using json = nlohmann::json;

#ifndef AGENT_TEST_A2A_ROOT
#define AGENT_TEST_A2A_ROOT "."
#endif

namespace af = agent_framework;
namespace a2a = af::a2a;
namespace h = agent_tests::a2a_contract;

static std::string bundle_dir() {
    return std::string(AGENT_TEST_A2A_ROOT) + "/synthetic-v1";
}

static void fail(const std::string& m) {
    std::cerr << "test_a2a_contract_json: " << m << "\n";
    std::exit(1);
}

static void check_jsonrpc_request(const std::string& path) {
    json j = h::read_json_file(path);
    std::string body = j.dump();
    a2a::JsonRpcParseResult pr = a2a::parse_jsonrpc_request(body);
    auto* req = std::get_if<a2a::JsonRpcRequest>(&pr);
    if (!req) {
        fail("parse_ok expected JsonRpcRequest for " + path);
    }
    if (req->method.empty()) {
        fail("empty method " + path);
    }
}

static void check_jsonrpc_expect_error(const std::string& body,
                                       const std::optional<int>& expected_code,
                                       const std::string& fixture_id) {
    a2a::JsonRpcParseResult pr = a2a::parse_jsonrpc_request(body);
    const json* err_obj = std::get_if<json>(&pr);
    if (!err_obj) {
        fail("expect_jsonrpc_error_response: got request for " + fixture_id);
    }
    auto code = a2a::try_get_jsonrpc_error_code(*err_obj);
    if (!code) {
        fail("expect_jsonrpc_error_response: missing error.code for " + fixture_id);
    }
    if (expected_code.has_value() && *code != *expected_code) {
        fail("expect_jsonrpc_error_response: code mismatch for " + fixture_id + " got " +
             std::to_string(*code));
    }
}

static void check_jsonrpc_envelope_ok(const std::string& path) {
    json j = h::read_json_file(path);
    if (j.value("jsonrpc", std::string()) != "2.0") {
        fail("jsonrpc 2.0 " + path);
    }
    if (!j.contains("result")) {
        fail("missing result " + path);
    }
}

static void check_jsonrpc_error_ok(const std::string& path) {
    json j = h::read_json_file(path);
    if (!j.contains("error") || !j["error"].is_object()) {
        fail("missing error object " + path);
    }
    if (!j["error"].contains("code")) {
        fail("missing error.code " + path);
    }
}

static void check_agent_card_roundtrip(const std::string& path) {
    json j = h::read_json_file(path);
    af::AgentCard c = a2a::agent_card_from_a2a_wire(j);
    json w = a2a::agent_card_to_a2a_wire(c);
    if (c.skills.empty()) {
        fail("card_with_skills expected non-empty skills");
    }
    const std::string normalized_url = w.at("supportedInterfaces").front().at("url");
    const std::string fixture_url = j.contains("supportedInterfaces")
        ? j.at("supportedInterfaces").front().at("url").get<std::string>()
        : j.at("url").get<std::string>();
    if (w["name"] != j["name"] || normalized_url != fixture_url) {
        fail("card round-trip name/url mismatch");
    }
}

static std::vector<std::string> read_ignore_keys(const json& entry) {
    std::vector<std::string> out;
    if (!entry.contains("ignore_json_keys") || !entry["ignore_json_keys"].is_array()) {
        return out;
    }
    for (const auto& k : entry["ignore_json_keys"]) {
        if (k.is_string()) {
            out.push_back(k.get<std::string>());
        }
    }
    return out;
}

static void check_task_roundtrip(const std::string& path, const std::vector<std::string>& ignore_keys) {
    json j = h::read_json_file(path);
    af::AgentTask t = a2a::task_from_a2a_wire(j);
    json out = a2a::task_to_a2a_wire(t);
    if (!h::json_equal_after_canonical(j, out, ignore_keys)) {
        fail("task round-trip canonical mismatch " + path);
    }
}

static std::optional<int> read_optional_error_code(const json& entry) {
    if (!entry.contains("expected_error_code")) {
        return std::nullopt;
    }
    const auto& c = entry["expected_error_code"];
    if (c.is_number_integer()) {
        return c.get<int>();
    }
    if (c.is_number_unsigned()) {
        return static_cast<int>(c.get<std::uint64_t>());
    }
    return std::nullopt;
}

static void check_task_dual_golden(const std::string& path_a,
                                   const std::string& path_b,
                                   const std::vector<std::string>& ignore_keys,
                                   const std::string& fixture_id) {
    json ja = h::read_json_file(path_a);
    json jb = h::read_json_file(path_b);
    if (!h::json_equal_after_canonical(ja, jb, ignore_keys)) {
        fail("equals_after_parse canonical mismatch for " + fixture_id);
    }
}

int main() {
    try {
        const std::string mpath = bundle_dir() + "/manifest.json";
        json manifest = h::read_json_file(mpath);
        if (!manifest.contains("fixtures") || !manifest["fixtures"].is_array()) {
            fail("manifest missing fixtures[]");
        }
        for (const auto& e : manifest["fixtures"]) {
            if (!e.contains("kind") || !e.contains("path") || !e.contains("assert")) {
                fail("fixture entry missing kind/path/assert");
            }
            const std::string fid = e.value("id", std::string("<no id>"));
            const std::string kind = e["kind"].get<std::string>();
            const std::string rel = e["path"].get<std::string>();
            const std::string assertv = e["assert"].get<std::string>();
            const std::string full = bundle_dir() + "/" + rel;
            if (kind == "jsonrpc_request" && assertv == "parse_ok") {
                check_jsonrpc_request(full);
            } else if (kind == "jsonrpc_request_raw" && assertv == "expect_jsonrpc_error_response") {
                std::string body = h::read_text_file(full);
                check_jsonrpc_expect_error(body, read_optional_error_code(e), fid);
            } else if (kind == "jsonrpc_request" && assertv == "expect_jsonrpc_error_response") {
                json j = h::read_json_file(full);
                check_jsonrpc_expect_error(j.dump(), read_optional_error_code(e), fid);
            } else if (kind == "jsonrpc_response" && assertv == "jsonrpc_envelope_ok") {
                check_jsonrpc_envelope_ok(full);
            } else if (kind == "jsonrpc_response" && assertv == "jsonrpc_error_ok") {
                check_jsonrpc_error_ok(full);
            } else if (kind == "agent_card_json" && assertv == "round_trip_types") {
                check_agent_card_roundtrip(full);
            } else if (kind == "task_wire" && assertv == "round_trip_types") {
                const auto ignores = read_ignore_keys(e);
                check_task_roundtrip(full, ignores);
                if (e.contains("equals_after_parse") && e["equals_after_parse"].is_string()) {
                    const std::string rel_b = e["equals_after_parse"].get<std::string>();
                    check_task_dual_golden(full, bundle_dir() + "/" + rel_b, ignores, fid);
                }
            } else if (kind == "sse_stream") {
                continue;
            } else {
                fail("unsupported fixture kind/assert: " + kind + " / " + assertv + " id=" + fid);
            }
        }
    } catch (const std::exception& ex) {
        fail(ex.what());
    }
    std::cout << "test_a2a_contract_json: ok\n";
    return 0;
}
