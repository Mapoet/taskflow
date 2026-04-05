/**
 * @file test_a2a_contract_json.cpp
 * @brief WP2.6 Tier A: drive synthetic-v1 manifest (JSON-RPC, Card, Task wire)
 */
#include <agent/a2a/jsonrpc.hpp>
#include <agent/a2a/wire_card.hpp>
#include <agent/a2a/wire_mapping.hpp>

#include "a2a_contract_helpers.hpp"

#include <cstdlib>
#include <iostream>
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
    if (w["name"] != j["name"] || w["url"] != j["url"]) {
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
            const std::string kind = e["kind"].get<std::string>();
            const std::string rel = e["path"].get<std::string>();
            const std::string assertv = e["assert"].get<std::string>();
            const std::string full = bundle_dir() + "/" + rel;
            if (kind == "jsonrpc_request" && assertv == "parse_ok") {
                check_jsonrpc_request(full);
            } else if (kind == "jsonrpc_response" && assertv == "jsonrpc_envelope_ok") {
                check_jsonrpc_envelope_ok(full);
            } else if (kind == "jsonrpc_response" && assertv == "jsonrpc_error_ok") {
                check_jsonrpc_error_ok(full);
            } else if (kind == "agent_card_json" && assertv == "round_trip_types") {
                check_agent_card_roundtrip(full);
            } else if (kind == "task_wire" && assertv == "round_trip_types") {
                check_task_roundtrip(full, read_ignore_keys(e));
            } else if (kind == "sse_stream") {
                continue;
            } else {
                fail("unsupported fixture kind/assert: " + kind + " / " + assertv);
            }
        }
    } catch (const std::exception& ex) {
        fail(ex.what());
    }
    std::cout << "test_a2a_contract_json: ok\n";
    return 0;
}
