/**
 * @file a2a_fixture_regen.cpp
 * @brief Optional maintainer tool: dump task wire round-trip JSON (AGENT_A2A_UPDATE_GOLDENS=1).
 */
#include <agent/a2a/wire_mapping.hpp>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#ifndef AGENT_TEST_A2A_ROOT
#define AGENT_TEST_A2A_ROOT "."
#endif

namespace af = agent_framework;
namespace a2a = af::a2a;

int main() {
    const char* u = std::getenv("AGENT_A2A_UPDATE_GOLDENS");
    if (u == nullptr || u[0] == '\0' || (u[0] == '0' && u[1] == '\0')) {
        std::cerr << "a2a_fixture_regen: set AGENT_A2A_UPDATE_GOLDENS=1 to emit round-trip JSON to stdout\n";
        return 0;
    }
    const std::string root = std::string(AGENT_TEST_A2A_ROOT) + "/synthetic-v1/jsonrpc/task_wire.json";
    std::ifstream in(root);
    if (!in) {
        std::cerr << "a2a_fixture_regen: missing " << root << "\n";
        return 1;
    }
    nlohmann::json j;
    in >> j;
    af::AgentTask t = a2a::task_from_a2a_wire(j);
    nlohmann::json out = a2a::task_to_a2a_wire(t);
    std::cout << out.dump(2) << "\n";
    return 0;
}
