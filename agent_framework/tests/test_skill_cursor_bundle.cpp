/**
 * @file test_skill_cursor_bundle.cpp
 * @brief 解析仓库内 `data/skills-cursor`（Cursor 形态 SKILL.md）golden 回归
 */

#include <agent/skills/skill_registry.hpp>

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>

#ifndef AGENT_TEST_SKILLS_CURSOR_ROOT
#error AGENT_TEST_SKILLS_CURSOR_ROOT must be set by CMake for this test.
#endif

namespace fs = std::filesystem;

int main() {
    using namespace agent_framework;
    const fs::path root = fs::path(AGENT_TEST_SKILLS_CURSOR_ROOT);
    if (!fs::exists(root) || !fs::is_directory(root)) {
        std::clog << "test_skill_cursor_bundle: skip (missing " << root.string() << ")\n";
        return 0;
    }

    SkillRegistry reg(root);
    reg.scan_or_reload();
    assert(reg.entries().size() >= 9U);

    const auto babysit = reg.get("babysit");
    assert(babysit.has_value());
    assert(babysit->description.find("merge-ready") != std::string::npos);
    assert(!babysit->disable_model_invocation);

    const auto shell_ent = reg.get("shell");
    assert(shell_ent.has_value());
    assert(shell_ent->disable_model_invocation);
    assert(!reg.match("shell").has_value());
    assert(!reg.match("/shell").has_value());

    const auto rule = reg.get("create-rule");
    assert(rule.has_value());
    assert(rule->description.size() > 50U);
    assert(rule->description.find("Cursor rules") != std::string::npos ||
           rule->description.find("rules") != std::string::npos);

    std::clog << "test_skill_cursor_bundle: ok (" << reg.entries().size() << " skills)\n";
    return 0;
}
