#include <agent/skills/skill_command.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

namespace fs = std::filesystem;
using namespace agent_framework;

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output.good());
    output << content;
}

fs::path make_package(const fs::path& root, const std::string& folder,
                      const std::string& version, const std::string& payload) {
    const auto package = root / folder;
    write_file(package / "SKILL.md",
        "---\napi-version: agent.taskflow/v1\nkind: Skill\nname: alpha\nversion: " +
        version + "\ndescription: lifecycle CLI fixture\nresources:\n  references:\n"
        "    - id: data\n      path: data.txt\n---\nalpha\n");
    write_file(package / "data.txt", payload);
    return package;
}

} // namespace

int main() {
    const auto base = fs::temp_directory_path() / "agent-skill-cli-lifecycle";
    fs::remove_all(base);
    const auto root = base / "root";
    const auto store = base / "store";
    const auto v100 = make_package(base / "sources", "alpha-100", "1.0.0", "one");
    const auto v110 = make_package(base / "sources", "alpha-110", "1.1.0", "two");
    auto registry = std::make_shared<SkillRegistry>(root);
    registry->scan_or_reload();
    SkillCommandService service(registry);

    assert(service.install({}, v100).exit == SkillCliExit::Usage);
    assert(service.install(store, base / "missing").exit == SkillCliExit::IntegrityFailed);

    const auto installed = service.install(store, v100, "file://alpha-100", "publisher:test");
    assert(installed.ok());
    assert(installed.data.at("package").at("id") == "alpha");
    assert(installed.data.at("package").at("packageDigest").get<std::string>().size() == 64);
    assert(installed.data.at("package").at("resourceDigests").contains("data"));
    assert(installed.data.contains("generation"));

    const auto bad_range = service.enable(store, "alpha", ">=1.0");
    assert(bad_range.exit == SkillCliExit::OperationFailed);
    assert(bad_range.error.at("code") == "skill_dependency_conflict");

    const auto enabled = service.enable(store, "alpha", "^1.0.0");
    assert(enabled.ok());
    assert(enabled.data.at("roots") == nlohmann::json::array({"alpha"}));
    assert(enabled.data.at("rootRanges").at("alpha") == "^1.0.0");
    assert(enabled.data.at("packages").size() == 1);
    const auto digest100 = enabled.data.at("packages").at(0).at("packageDigest").get<std::string>();
    assert(service.remove(store, digest100).exit == SkillCliExit::OperationFailed);

    const auto updated = service.update(store, v110, "file://alpha-110", "publisher:test");
    assert(updated.ok());
    assert(updated.data.at("packages").at(0).at("version") == "1.1.0");

    const auto rolled_back = service.rollback(store, "alpha");
    assert(rolled_back.ok());
    assert(rolled_back.data.at("packages").at(0).at("version") == "1.0.0");

    const auto disabled = service.disable(store, "alpha");
    assert(disabled.ok());
    assert(disabled.data.at("roots").empty());
    assert(service.remove(store, digest100).ok());
    assert(service.remove(store, "bad-digest").exit == SkillCliExit::IntegrityFailed);
    assert(service.remove(store, std::string(64, 'f')).exit == SkillCliExit::NotFound);
    assert(service.disable(store, "missing").exit == SkillCliExit::NotFound);
    assert(service.rollback(store, "missing").exit == SkillCliExit::NotFound);

    const auto corrupt_store = base / "corrupt-store";
    write_file(corrupt_store / "state" / "skills.lock", "not-json\n");
    assert(service.enable(corrupt_store, "alpha").exit == SkillCliExit::IntegrityFailed);

    fs::remove_all(base);
    std::cout << "skill CLI lifecycle contract tests passed\n";
    return 0;
}
