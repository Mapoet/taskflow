#include <agent/skill_package_gate.hpp>
#include <agent/skill_command.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sys/stat.h>

namespace {

namespace fs = std::filesystem;
using namespace agent_framework;

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output.good());
    output << content;
}

fs::path make_package(const fs::path& root, const std::string& name,
                      const std::string& expected = "value") {
    const auto package = root / name;
    write_file(package / "data.txt", "value");
    write_file(package / "tests/pass.json",
        "{\"apiVersion\":\"agent.taskflow/skill-test/v1\",\"kind\":\"SkillTest\","
        "\"name\":\"package preflight\",\"target\":{\"kind\":\"resource\","
        "\"resource\":\"data\"},\"expect\":{\"ok\":true,\"output\":\"" + expected + "\"}}");
    write_file(package / "SKILL.md",
        "---\napi-version: agent.taskflow/v1\nkind: Skill\nname: " + name +
        "\nversion: 1.0.0\ndescription: package gate fixture\nresources:\n  references:\n"
        "    - id: data\n      path: data.txt\n      media-type: text/plain\n"
        "  tests:\n    - id: pass\n      path: tests/pass.json\n---\nfixture\n");
    return package;
}

std::map<std::string, std::string> snapshot_files(const fs::path& root) {
    std::map<std::string, std::string> files;
    std::error_code error;
    if(!fs::exists(root)) return files;
    for(const auto& item : fs::recursive_directory_iterator(root, error)) {
        assert(!error);
        if(!item.is_regular_file()) continue;
        std::ifstream input(item.path(), std::ios::binary);
        files[fs::relative(item.path(), root).generic_string()] =
            std::string(std::istreambuf_iterator<char>(input), {});
    }
    return files;
}

} // namespace

int main() {
    const auto base = fs::temp_directory_path() / "agent-skill-package-gate";
    fs::remove_all(base);
    const auto package = make_package(base, "quality");
    SkillPackageGate gate;

    const auto first = gate.inspect(package);
    const auto second = gate.inspect(package);
    assert(first.ok && second.ok && first.package && second.package);
    assert(first.package->package_digest == second.package->package_digest);
    assert(first.package->resource_digests == second.package->resource_digests);
    assert(first.to_json().at("package") == second.to_json().at("package"));
    assert(!fs::exists(base / "store"));

    const auto failing = make_package(base, "failing", "wrong");
    const auto failed_tests = gate.inspect(failing);
    assert(!failed_tests.ok);
    assert(failed_tests.error.at("code") == "skill_package_tests_failed");

    auto registry = std::make_shared<SkillRegistry>(std::vector<fs::path>{});
    SkillCommandService service(registry);
    const auto store = base / "store";
    const auto generation_before = registry->snapshot().generation();
    assert(service.install(store, failing).exit == SkillCliExit::ContractFailed);
    assert(!fs::exists(store));
    assert(registry->snapshot().generation() == generation_before);
    assert(service.install(store, package).ok());
    const auto stored = snapshot_files(store);
    const auto generation_after_install = registry->snapshot().generation();
    assert(service.update(store, failing).exit == SkillCliExit::ContractFailed);
    assert(snapshot_files(store) == stored);
    assert(registry->snapshot().generation() == generation_after_install);

    const auto missing = make_package(base, "missing");
    fs::remove(missing / "data.txt");
    assert(!gate.inspect(missing).ok);

    const auto mismatch = make_package(base, "mismatch");
    auto manifest = std::string("---\napi-version: agent.taskflow/v1\nkind: Skill\nname: mismatch\n") +
        "version: 1.0.0\ndescription: digest mismatch\nresources:\n  references:\n"
        "    - id: data\n      path: data.txt\n      sha256: " + std::string(64, 'f') +
        "\n---\nfixture\n";
    write_file(mismatch / "SKILL.md", manifest);
    const auto mismatched = gate.inspect(mismatch);
    assert(!mismatched.ok && mismatched.error.at("code") == kSkillDigestMismatch);
    assert(service.update(store, mismatch).exit == SkillCliExit::IntegrityFailed);
    assert(snapshot_files(store) == stored);
    assert(registry->snapshot().generation() == generation_after_install);

    const auto traversal = make_package(base, "traversal");
    write_file(traversal / "SKILL.md",
        "---\napi-version: agent.taskflow/v1\nkind: Skill\nname: traversal\nversion: 1.0.0\n"
        "description: traversal\nresources:\n  references:\n    - id: data\n"
        "      path: ../outside.txt\n---\nfixture\n");
    assert(!gate.inspect(traversal).ok);

    const auto linked = make_package(base, "linked");
    fs::create_symlink(linked / "data.txt", linked / "alias.txt");
    assert(!gate.inspect(linked).ok);

    const auto special = make_package(base, "special");
    assert(::mkfifo((special / "pipe").c_str(), 0600) == 0);
    assert(!gate.inspect(special).ok);

    fs::remove_all(base);
    std::cout << "skill package gate tests passed\n";
    return 0;
}
