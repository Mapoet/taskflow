#include <agent/skills/skill_manager.hpp>
#include <agent/skills/skill_loader.hpp>
#include <agent/skills/skill_runtime.hpp>
#include <agent/toolbus/toolbus.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using namespace agent_framework;

namespace {
void write(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << value;
}
}

int main() {
    const auto root = fs::temp_directory_path() / "agent-skill-manager";
    std::error_code ec;
    fs::remove_all(root, ec);
    write(root / "base/SKILL.md", R"(---
api-version: agent.taskflow/v1
kind: Skill
name: base
version: 1.0.0
description: Manager fixture
permissions:
  tools: []
resources: {}
---
Body
)");
    auto registry = std::make_shared<SkillRegistry>(root);
    registry->scan_or_reload();
    auto loader = std::make_shared<SkillLoader>(*registry);
    auto runtime = std::make_shared<SkillRuntime>(registry, loader);
    auto manager = std::make_shared<SkillManager>(registry, loader, runtime, root);
    auto bus = std::make_shared<ToolBus>();
    manager->attach_toolbus(bus);

    assert(manager->list()["skills"].size() == 1);
    assert(manager->validate("base").value("ok", false));
    assert(!manager->validate("missing").value("ok", true));
    auto created = manager->create("new-skill", "Created: safely\nwithout YAML injection");
    assert(created.value("ok", false));
    assert(!manager->create("../escape", "bad").value("ok", true));
    assert(manager->reload().value("ok", false));
    assert(manager->validate("new-skill").value("ok", false));

    auto activated = manager->activate("base");
    assert(activated.value("ok", false));
    assert(manager->active_skill_id() == "base");
    assert(!manager->reload().value("ok", true));
    assert(manager->deactivate().value("ok", false));
    assert(manager->active_skill_id().empty());
    std::cout << "test_skill_manager: ok\n";
}
