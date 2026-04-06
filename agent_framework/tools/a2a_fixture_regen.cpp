/**
 * @file a2a_fixture_regen.cpp
 * @brief Maintainer tool: task wire round-trip JSON (AGENT_A2A_UPDATE_GOLDENS=1 required to write).
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

static void usage() {
    std::cerr << "Usage: a2a_fixture_regen --in <path.json> [--out <path.json>]\n"
              << "  Requires AGENT_A2A_UPDATE_GOLDENS=1 to write --out (otherwise no-op).\n"
              << "  Without --out: prints round-trip JSON to stdout.\n"
              << "  Default --in: " << AGENT_TEST_A2A_ROOT << "/synthetic-v1/jsonrpc/task_wire.json\n";
}

int main(int argc, char** argv) {
    const char* u = std::getenv("AGENT_A2A_UPDATE_GOLDENS");
    const bool allow_write =
        u != nullptr && u[0] != '\0' && !(u[0] == '0' && u[1] == '\0');

    std::string in_path = std::string(AGENT_TEST_A2A_ROOT) + "/synthetic-v1/jsonrpc/task_wire.json";
    std::string out_path;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            usage();
            return 0;
        }
        if (a == "--in" && i + 1 < argc) {
            in_path = argv[++i];
        } else if (a == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        } else {
            std::cerr << "a2a_fixture_regen: unknown arg: " << a << "\n";
            usage();
            return 1;
        }
    }

    std::ifstream in(in_path);
    if (!in) {
        std::cerr << "a2a_fixture_regen: cannot open " << in_path << "\n";
        return 1;
    }
    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& ex) {
        std::cerr << "a2a_fixture_regen: parse failed: " << ex.what() << "\n";
        return 1;
    }
    af::AgentTask t = a2a::task_from_a2a_wire(j);
    nlohmann::json out = a2a::task_to_a2a_wire(t);

    if (!out_path.empty()) {
        if (!allow_write) {
            std::cerr << "a2a_fixture_regen: refusing to write " << out_path
                      << " (set AGENT_A2A_UPDATE_GOLDENS=1)\n";
            return 1;
        }
        std::ofstream ofs(out_path, std::ios::out | std::ios::trunc);
        if (!ofs) {
            std::cerr << "a2a_fixture_regen: cannot write " << out_path << "\n";
            return 1;
        }
        ofs << out.dump(2) << "\n";
        std::cerr << "a2a_fixture_regen: wrote " << out_path << "\n";
        return 0;
    }

    if (!allow_write) {
        std::cerr << "a2a_fixture_regen: set AGENT_A2A_UPDATE_GOLDENS=1 for write mode; "
                     "printing round-trip to stdout (read-only)\n";
    }
    std::cout << out.dump(2) << "\n";
    return 0;
}
