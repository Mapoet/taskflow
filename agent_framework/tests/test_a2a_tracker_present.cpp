/**
 * @file test_a2a_tracker_present.cpp
 * @brief 门禁：a2a-spec-tracker.md 关键章节存在（WP2.1a 可选）
 */

#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifndef AGENT_TEST_A2A_TRACKER_PATH
#define AGENT_TEST_A2A_TRACKER_PATH "docs/guides/a2a-spec-tracker.md"
#endif

int main() {
    std::ifstream in(AGENT_TEST_A2A_TRACKER_PATH);
    if (!in) {
        std::cerr << "missing tracker: " << AGENT_TEST_A2A_TRACKER_PATH << "\n";
        return 2;
    }
    std::stringstream buf;
    buf << in.rdbuf();
    std::string content = buf.str();
    assert(content.find("## 4.") != std::string::npos);
    assert(content.find("JSON-RPC") != std::string::npos);
    assert(content.find("a2a-spec-tracker.md") != std::string::npos || content.find("Well-Known") != std::string::npos);
    std::cout << "a2a tracker present ok\n";
    return 0;
}
