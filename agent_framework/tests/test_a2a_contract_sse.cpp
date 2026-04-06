/**
 * @file test_a2a_contract_sse.cpp
 * @brief WP2.6 Tier A: SSE fixtures from synthetic-v1 manifest
 */
#include <agent/a2a/sse_framing.hpp>

#include "a2a_contract_helpers.hpp"

#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef AGENT_TEST_A2A_ROOT
#define AGENT_TEST_A2A_ROOT "."
#endif

namespace h = agent_tests::a2a_contract;
using json = nlohmann::json;

static void fail(const std::string& m) {
    std::cerr << "test_a2a_contract_sse: " << m << "\n";
    std::exit(1);
}

static std::string bundle_dir() {
    return std::string(AGENT_TEST_A2A_ROOT) + "/synthetic-v1";
}

int main() {
    try {
        json manifest = h::read_json_file(bundle_dir() + "/manifest.json");
        for (const auto& e : manifest["fixtures"]) {
            if (!e.contains("kind") || e["kind"].get<std::string>() != "sse_stream") {
                continue;
            }
            const std::string assertv = e.contains("assert") ? e["assert"].get<std::string>() : std::string();
            if (assertv != "sse_parse_ok" && assertv != "sse_event_count") {
                fail("unsupported sse assert: " + assertv);
            }
            const std::string rel = e["path"].get<std::string>();
            const std::string full = bundle_dir() + "/" + rel;
            std::string raw = h::read_text_file(full);
            agent_framework::a2a::SseParser parser;
            parser.feed(raw);
            std::vector<agent_framework::a2a::SseEvent> evs;
            parser.drain_events(evs);
            std::size_t min_ev = 1;
            if (e.contains("expected_min_events") && e["expected_min_events"].is_number_unsigned()) {
                min_ev = e["expected_min_events"].get<std::size_t>();
            }
            if (evs.size() < min_ev) {
                fail("too few SSE events in " + full);
            }
            if (e.contains("expected_event_names") && e["expected_event_names"].is_array()) {
                std::vector<std::string> expected;
                for (const auto& n : e["expected_event_names"]) {
                    if (n.is_string()) {
                        expected.push_back(n.get<std::string>());
                    }
                }
                if (expected.size() != evs.size()) {
                    fail("expected_event_names count mismatch in " + full);
                }
                for (std::size_t i = 0; i < evs.size(); ++i) {
                    if (evs[i].event != expected[i]) {
                        fail("SSE event name mismatch at index " + std::to_string(i) + " in " + full);
                    }
                }
            }
            for (const auto& ev : evs) {
                if (ev.data.empty()) {
                    fail("empty SSE data in " + full);
                }
                try {
                    auto parsed = json::parse(ev.data);
                    (void)parsed;
                } catch (...) {
                    fail("invalid JSON in SSE data");
                }
            }
        }
    } catch (const std::exception& ex) {
        fail(ex.what());
    }
    std::cout << "test_a2a_contract_sse: ok\n";
    return 0;
}
