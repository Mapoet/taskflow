#include <agent/resources/resource_uri.hpp>
#include <agent/resources/session_resource_context.hpp>
#include <agent/skills/skill_registry.hpp>

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
bool rejects(const std::string& value) {
    try { (void)ResourceUri::parse(value); return false; } catch (...) { return true; }
}
}

int main() {
    const auto workspace = fs::temp_directory_path() / "agent-resource-runtime-workspace";
    const auto skills = fs::temp_directory_path() / "agent-resource-runtime-skills";
    const auto cache = fs::temp_directory_path() / "agent-resource-runtime-cache";
    std::error_code ec;
    fs::remove_all(workspace, ec); fs::remove_all(skills, ec); fs::remove_all(cache, ec);
    write(workspace / "docs/input.md", "workspace");
    write(skills / "sample/references/data.md", "skill");
    write(skills / "sample/SKILL.md", R"(---
name: sample
description: Resource fixture
references: [references/data.md]
---
Body
)");
    SkillRegistry registry(skills);
    registry.scan_or_reload();
    assert(registry.get("sample"));

    const auto workspace_uri = ResourceUri::parse("workspace://docs/input.md");
    const auto skill_uri = ResourceUri::parse("skill://sample/references/data.md");
    assert(workspace_uri.str() == "workspace://docs/input.md");
    assert(skill_uri.authority() == "sample");
    assert(rejects("file:///etc/passwd"));
    assert(rejects("workspace://../secret"));
    assert(rejects("skill://sample/../../secret"));
    assert(rejects("mcp://service"));
    const auto opaque_mcp = ResourceUri::parse("mcp://filesystem/file:///home/data.txt");
    assert(opaque_mcp.authority() == "filesystem");
    assert(opaque_mcp.path() == "file:///home/data.txt");
    assert(opaque_mcp.str() == "mcp://filesystem/file:///home/data.txt");
    assert(rejects(std::string("mcp://filesystem/memory://bad\nvalue")));

    SessionResourceContext context(workspace, {skills}, {}, cache, registry.snapshot());
    assert(context.resolve_local(workspace_uri) == fs::weakly_canonical(workspace / "docs/input.md"));
    assert(context.resolve_local(skill_uri) == fs::weakly_canonical(skills / "sample/references/data.md"));
    assert(context.mcp_allowed("anything"));
    context.set_allowed_mcp_services({"filesystem"});
    assert(context.mcp_allowed("filesystem"));
    assert(!context.mcp_allowed("github"));
    bool mcp_local_rejected = false;
    try { (void)context.resolve_local(ResourceUri::parse("mcp://filesystem/root")); }
    catch (...) { mcp_local_rejected = true; }
    assert(mcp_local_rejected);
    std::cout << "test_resource_runtime: ok\n";
}
