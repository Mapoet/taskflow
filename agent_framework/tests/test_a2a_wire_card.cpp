/**
 * @file test_a2a_wire_card.cpp
 * @brief WP2.1a Agent Card wire 单测 C-1–C-3、skills round-trip
 */

#include <agent/a2a/wire_card.hpp>

#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

using agent_framework::AgentCard;
using agent_framework::AgentSkill;
using agent_framework::a2a::agent_card_discovery_json_string;
using agent_framework::a2a::agent_card_from_a2a_wire;
using agent_framework::a2a::agent_card_to_a2a_wire;

#ifndef AGENT_TEST_A2A_FIXTURE_DIR
#define AGENT_TEST_A2A_FIXTURE_DIR "."
#endif

static void assert_card_equal(const AgentCard& a, const AgentCard& b) {
    assert(a.name == b.name);
    assert(a.description == b.description);
    assert(a.provider == b.provider);
    assert(a.api_endpoint == b.api_endpoint);
    assert(a.capabilities == b.capabilities);
    assert(a.authentication_scheme == b.authentication_scheme);
    assert(a.skills.size() == b.skills.size());
    for (std::size_t i = 0; i < a.skills.size(); ++i) {
        assert(a.skills[i].name == b.skills[i].name);
        assert(a.skills[i].description == b.skills[i].description);
        assert(a.skills[i].required_capabilities == b.skills[i].required_capabilities);
    }
}

static void c1_min_roundtrip() {
    AgentCard c;
    c.name = "MinAgent";
    c.description = "desc";
    c.provider = "Org";
    c.api_endpoint = "https://ex.test/rpc";
    c.capabilities = {"streaming", "push-notifications"};
    c.authentication_scheme = json::object({{"noop", json::object()}});
    c.skills.clear();

    json w = agent_card_to_a2a_wire(c);
    AgentCard back = agent_card_from_a2a_wire(w);
    assert_card_equal(c, back);
}

static void c2_fixture_normalized() {
    std::string path =
        std::string(AGENT_TEST_A2A_FIXTURE_DIR) + "/tests/fixtures/a2a/card_min.json";
    std::ifstream in(path);
    assert(in && "open fixture");
    std::stringstream buf;
    buf << in.rdbuf();
    json fixture = json::parse(buf.str());

    AgentCard from_f = agent_card_from_a2a_wire(fixture);
    assert(from_f.name == "FixtureAgent");
    assert(from_f.api_endpoint == "https://example.org/rpc");
    assert(from_f.capabilities.size() == 1 && from_f.capabilities[0] == "streaming");

    AgentCard manual;
    manual.name = "FixtureAgent";
    manual.description = "Fixture agent for WP2.1a";
    manual.provider = "";
    manual.api_endpoint = "https://example.org/rpc";
    manual.capabilities = {"streaming"};
    manual.authentication_scheme = json::object();
    manual.skills.clear();

    json a = agent_card_to_a2a_wire(from_f);
    json b = agent_card_to_a2a_wire(manual);
    assert(a == b);
}

static void c3_missing_required() {
    json j = agent_card_to_a2a_wire([] {
        AgentCard c;
        c.name = "x";
        c.description = "d";
        c.provider = "";
        c.api_endpoint = "https://u";
        c.capabilities = {};
        c.authentication_scheme = json::object();
        return c;
    }());
    j.erase("supportedInterfaces");
    try {
        (void)agent_card_from_a2a_wire(j);
        assert(false);
    } catch (const std::invalid_argument&) {
    }
}

static void skills_roundtrip(std::size_t n) {
    AgentCard c;
    c.name = "S";
    c.description = "d";
    c.provider = "p";
    c.api_endpoint = "https://s/rpc";
    c.capabilities = {};
    c.authentication_scheme = json::object();
    for (std::size_t i = 0; i < n; ++i) {
        AgentSkill sk;
        sk.name = "skill_" + std::to_string(i);
        sk.description = "sd" + std::to_string(i);
        sk.required_capabilities = {std::string("t") + std::to_string(i)};
        sk.input_schema = json::object();
        sk.output_schema = json::object();
        c.skills.push_back(std::move(sk));
    }
    assert_card_equal(c, agent_card_from_a2a_wire(agent_card_to_a2a_wire(c)));
}

static void discovery_string_parseable() {
    AgentCard c;
    c.name = "D";
    c.description = "d";
    c.provider = "";
    c.api_endpoint = "https://d";
    c.capabilities = {};
    c.authentication_scheme = json::object();
    std::string s = agent_card_discovery_json_string(c);
    json parsed = json::parse(s);
    (void)agent_card_from_a2a_wire(parsed);
}

int main() {
    c1_min_roundtrip();
    c2_fixture_normalized();
    c3_missing_required();
    skills_roundtrip(0);
    skills_roundtrip(1);
    skills_roundtrip(3);
    discovery_string_parseable();
    std::cout << "a2a wire_card C-1..C-3 + skills ok\n";
    return 0;
}
