#include <agent/skill_lifecycle.hpp>
#include <agent/skill_loader.hpp>
#include <agent/skill_workflow.hpp>

#include <atomic>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>

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
                      const std::string& id, const std::string& version,
                      const std::string& payload,
                      const std::string& dependency = {},
                      const std::string& range = {}) {
    const fs::path package = root / folder;
    fs::create_directories(package);
    std::string manifest = "---\napi-version: agent.taskflow/v1\nkind: Skill\nname: " + id +
        "\nversion: " + version + "\ndescription: lifecycle test package\n";
    if (!dependency.empty()) {
        manifest += "dependencies:\n  - name: " + dependency + "\n    version: " + range + "\n";
    }
    manifest += "resources:\n  references:\n    - id: data\n      path: data.txt\n---\nbody\n";
    write_file(package / "SKILL.md", manifest);
    write_file(package / "data.txt", payload);
    return package;
}

fs::path make_workflow_package(const fs::path& root) {
    const fs::path package = root / "root-100";
    write_file(package / "tools/value.json", R"({"source":"base_value"})");
    write_file(package / "workflows/main.json", R"({
      "api-version":"agent.taskflow/workflow/v1","kind":"SkillWorkflow",
      "nodes":[{"id":"value","type":"tool","resource":"value",
        "input":{"value":{"from":"$input","path":"/value"}}}],
      "outputs":{"value":{"from":"value","path":"/value"}}
    })");
    write_file(package / "SKILL.md", R"(---
api-version: agent.taskflow/v1
kind: Skill
name: root
version: 1.0.0
description: lifecycle workflow root
dependencies:
  - name: dep
    version: ^1.0.0
permissions:
  tools: [base_value, skill::root::value]
  filesystem:
    read: [.]
resources:
  tools:
    - id: value
      path: tools/value.json
  workflows:
    - id: main
      path: workflows/main.json
---
root
)");
    return package;
}

SkillPackageRecord candidate(const std::string& id, const std::string& version,
                             std::vector<SkillDependency> dependencies = {}) {
    SkillPackageRecord record;
    record.id = id;
    record.version = *SkillSemVersion::parse(version);
    record.package_digest = std::string(64, id.empty() ? '0' : id.front());
    auto manifest = std::make_shared<SkillManifest>();
    manifest->name = id;
    manifest->version = version;
    manifest->dependencies = std::move(dependencies);
    record.manifest = std::move(manifest);
    return record;
}

void test_semver_and_resolution() {
    auto alpha = SkillSemVersion::parse("1.2.3-alpha.1+build.7");
    auto release = SkillSemVersion::parse("1.2.3");
    assert(alpha && release && *alpha < *release);
    assert(!SkillSemVersion::parse("01.2.3"));
    assert(!SkillSemVersion::parse("1.2.3-alpha!"));
    assert(!SkillSemVersion::parse("1.2.3-alpha."));
    assert(!SkillSemVersion::parse("1.2.3+build..7"));
    assert(!SkillSemVersion::parse("1.2.3+one+two"));
    assert(SkillSemVersionRange::parse("^1.2.0")->contains(*release));
    assert(!SkillSemVersionRange::parse("^2.0.0")->contains(*release));
    assert(SkillSemVersionRange::parse("~1.2.0 || >=2.0.0 <3.0.0")
               ->contains(*SkillSemVersion::parse("2.4.0")));
    assert(!SkillSemVersionRange::parse(">=1.0.0")->contains(*alpha));
    assert(!SkillSemVersionRange::parse(">=1.2"));
    assert(!SkillSemVersionRange::parse("^1.0.0 ||"));

    auto build_a = candidate("build", "1.0.0+z");
    auto build_b = candidate("build", "1.0.0+a");
    build_a.package_digest = std::string(64, 'f');
    build_b.package_digest = std::string(64, '0');
    SkillDependencyResolver::Catalog deterministic_catalog;
    deterministic_catalog["build"] = {build_a, build_b};
    auto deterministic = SkillDependencyResolver(deterministic_catalog).resolve(
        {{"$root", "build", "=1.0.0", false, {"build"}}});
    assert(deterministic.ok);
    assert(deterministic.packages.at("build").package_digest == std::string(64, '0'));

    SkillDependencyResolver::Catalog conflict_catalog;
    conflict_catalog["dep"] = {candidate("dep", "1.5.0"), candidate("dep", "2.1.0")};
    SkillDependencyResolver conflict_resolver(conflict_catalog);
    auto conflict = conflict_resolver.resolve({
        {"a", "dep", "^1.0.0", false, {"a", "dep"}},
        {"b", "dep", "^2.0.0", false, {"b", "dep"}}});
    assert(!conflict.ok);
    assert(conflict.error.value("code", "") == kSkillDependencyConflict);
    assert(conflict.conflict.size() == 2);

    SkillDependencyResolver::Catalog cycle_catalog;
    cycle_catalog["a"] = {candidate("a", "1.0.0", {{"b", "^1.0.0", false}})};
    cycle_catalog["b"] = {candidate("b", "1.0.0", {{"a", "^1.0.0", false}})};
    auto cycle = SkillDependencyResolver(cycle_catalog).resolve(
        {{"$root", "a", "*", false, {"a"}}});
    assert(!cycle.ok);
    assert(!cycle.conflict.empty());
    assert(cycle.conflict.front().path == std::vector<std::string>({"a", "b", "a"}));
}

void test_lockfile_validation() {
    SkillLockfile valid;
    valid.roots = {"root"};
    valid.packages.push_back(
        {"root", "1.0.0", std::string(64, 'a'), {{"data", std::string(64, 'b')}},
         "file://root", "publisher:test"});
    auto value = valid.to_json();
    assert(SkillLockfile::from_json(value));
    value["packages"][0]["resourceDigests"]["data"] = "not-a-digest";
    assert(!SkillLockfile::from_json(value));
    value = valid.to_json();
    value["roots"].push_back("missing");
    assert(!SkillLockfile::from_json(value));
    value = valid.to_json();
    value["packages"].push_back(value["packages"][0]);
    assert(!SkillLockfile::from_json(value));
}

void test_atomic_registry_reload(const fs::path& base) {
    const fs::path roots = base / "scan";
    auto package = make_package(roots, "stable", "stable", "1.0.0", "old");
    SkillRegistry registry(roots);
    registry.scan_or_reload();
    const auto before = registry.snapshot();
    assert(before.valid() && before.get("stable"));
    write_file(package / "SKILL.md", "---\napi-version: bad\nkind: Skill\nname: stable\nversion: 1.0.0\n---\n");
    registry.scan_or_reload();
    const auto failed = registry.snapshot();
    assert(!failed.valid());
    assert(failed.generation() == before.generation());
    assert(failed.get("stable") && failed.get("stable")->version == "1.0.0");
}

void test_lifecycle(const fs::path& base) {
    const fs::path sources = base / "sources";
    const fs::path store = base / "store";
    const auto dep150 = make_package(sources, "dep-150", "dep", "1.5.0", "dep-150");
    const auto dep200 = make_package(sources, "dep-200", "dep", "2.0.0", "dep-200");
    const auto root100 = make_workflow_package(sources);
    fs::create_directories(store / "transactions" / "orphan.tmp");
    write_file(store / "transactions" / "orphan.tmp" / "partial", "partial");

    auto registry = std::make_shared<SkillRegistry>(base / "unused");
    SkillLifecycleManager lifecycle(registry, store);
    assert(!fs::exists(store / "transactions" / "orphan.tmp"));

    auto dep150_install = lifecycle.install(dep150, {"file://dep-150", "publisher:test"});
    auto dep200_install = lifecycle.install(dep200, {"file://dep-200", "publisher:test"});
    auto root_install = lifecycle.install(root100, {"file://root-100", "publisher:test"});
    assert(dep150_install.ok && dep200_install.ok && root_install.ok);
    assert(dep150_install.package->package_digest != dep200_install.package->package_digest);

    auto enabled = lifecycle.enable("root", "^1.0.0");
    assert(enabled.ok && enabled.lockfile);
    auto initial = registry->snapshot();
    assert(initial.entries().size() == 2);
    assert(initial.get("root") && initial.get("dep"));
    assert(initial.get("dep")->version == "1.5.0");
    assert(initial.get("dep")->source_uri == "file://dep-150");
    const std::string stable_lock = enabled.lockfile->dump();
    assert(stable_lock == lifecycle.lockfile().dump());
    auto replayed = SkillLockfile::load(store / "state" / "skills.lock");
    assert(replayed && replayed->dump() == stable_lock);

    SkillLoader loader(*registry);
    auto old_payload = loader.load_resource_snapshot(
        *initial.get("dep"), initial.get_manifest("dep"), "data.txt",
        SkillResourceKind::Reference, 1024);
    assert(old_payload && *old_payload == "dep-150");

    const auto dep160 = make_package(sources, "dep-160", "dep", "1.6.0", "dep-160");
    auto shared_loader = std::make_shared<SkillLoader>(*registry);
    auto runtime = std::make_shared<SkillRuntime>(registry, shared_loader);
    auto bus = std::make_shared<ToolBus>();
    std::atomic_bool entered{false};
    std::atomic_bool release{false};
    ToolMeta metadata;
    metadata.side_effect = ToolSideEffect::ReadOnly;
    metadata.schema = {
        {"type", "object"},
        {"properties", {{"value", {{"type", "integer"}}}}},
        {"required", {"value"}},
        {"additionalProperties", false}
    };
    bus->register_local_tool("base_value", [&](const nlohmann::json& input) {
        entered.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        return input;
    }, metadata);
    auto capabilities = std::make_shared<SkillCapabilityRuntime>(
        registry, shared_loader, runtime, bus);
    SkillWorkflowRuntime workflows(registry, shared_loader, runtime, capabilities);
    SkillWorkflowRunOptions run_options;
    run_options.context.control = std::make_shared<TaskControl>();
    run_options.context.grants.tools = {"base_value", "skill::root::value"};
    run_options.context.grants.filesystem_read = {
        initial.get("root")->script_jail->string()};
    run_options.context.task_id = "stage5-trace";
    run_options.context.run_id = "stage5-old-run";
    auto old_run = std::async(std::launch::async, [&] {
        return workflows.run("root", "main", {{"value", 7}}, run_options);
    });
    while (!entered.load(std::memory_order_acquire)) {
        if (old_run.wait_for(std::chrono::milliseconds(1)) == std::future_status::ready) {
            const auto early = old_run.get();
            std::cerr << "stage5: old workflow failed early: " << early.error.dump() << "\n";
            assert(false && "old workflow must reach the blocking tool");
        }
        std::this_thread::yield();
    }
    auto updated = lifecycle.update(dep160, {"file://dep-160", "publisher:test"});
    release.store(true, std::memory_order_release);
    auto old_run_result = old_run.get();
    assert(updated.ok && registry->snapshot().get("dep")->version == "1.6.0");
    assert(old_run_result.ok && old_run_result.dependency_lock.at("dep") == "1.5.0");
    run_options.context.run_id = "stage5-new-run";
    auto new_run_result = workflows.run("root", "main", {{"value", 8}}, run_options);
    assert(new_run_result.ok && new_run_result.dependency_lock.at("dep") == "1.6.0");
    assert(initial.get("dep")->version == "1.5.0");
    old_payload = loader.load_resource_snapshot(
        *initial.get("dep"), initial.get_manifest("dep"), "data.txt",
        SkillResourceKind::Reference, 1024);
    assert(old_payload && *old_payload == "dep-150");

    auto rolled_back = lifecycle.rollback("dep");
    assert(rolled_back.ok && registry->snapshot().get("dep")->version == "1.5.0");

    const auto dep170 = make_package(sources, "dep-170", "dep", "1.7.0", "dep-170");
    std::atomic_bool stop{false};
    std::atomic_bool consistent{true};
    std::thread reader([&] {
        while (!stop.load(std::memory_order_acquire)) {
            const auto snapshot = registry->snapshot();
            const auto root = snapshot.get("root");
            const auto dep = snapshot.get("dep");
            if (!root || !dep || (dep->version != "1.5.0" && dep->version != "1.7.0"))
                consistent.store(false, std::memory_order_release);
        }
    });
    auto concurrent_update = lifecycle.update(dep170, {"file://dep-170", "publisher:test"});
    stop.store(true, std::memory_order_release);
    reader.join();
    assert(concurrent_update.ok && consistent.load());
    assert(registry->snapshot().get("dep")->version == "1.7.0");

    auto active = registry->snapshot();
    const std::string active_digest = active.get("dep")->package_digest;
    assert(!lifecycle.remove("../state").ok);
    assert(!lifecycle.remove(active_digest).ok);
    auto disabled = lifecycle.disable("root");
    assert(disabled.ok && registry->snapshot().entries().empty());
    assert(initial.get("dep") && active.get("dep"));
    assert(!lifecycle.remove(active_digest).ok);
    active = SkillRegistrySnapshot{};
    initial = SkillRegistrySnapshot{};
    concurrent_update = SkillLifecycleResult{};
    updated = SkillLifecycleResult{};
    assert(lifecycle.remove(active_digest).ok);

    // A fresh manager replays exact lock identities and cleans interrupted transactions.
    assert(lifecycle.enable("root", "^1.0.0").ok);
    const auto before_restart = lifecycle.lockfile().dump();
    fs::create_directories(store / "transactions" / "restart-orphan.tmp");
    auto restarted_registry = std::make_shared<SkillRegistry>(base / "unused-2");
    SkillLifecycleManager restarted(restarted_registry, store);
    assert(restarted.lockfile().dump() == before_restart);
    assert(restarted_registry->snapshot().get("dep"));
    assert(!fs::exists(store / "transactions" / "restart-orphan.tmp"));
    const auto root110 = make_package(sources, "root-110", "root", "1.1.0", "root-110");
    assert(restarted.update(root110).ok);
    assert(restarted_registry->snapshot().get("root")->version == "1.1.0");
}

void test_lock_provenance_replay(const fs::path& base) {
    const fs::path sources = base / "provenance-sources";
    const fs::path store = base / "provenance-store";
    const auto package = make_package(sources, "root", "root", "1.0.0", "payload");
    auto registry = std::make_shared<SkillRegistry>(base / "provenance-unused");
    SkillLifecycleManager lifecycle(registry, store);
    assert(lifecycle.install(package, {"file://trusted", "publisher:trusted"}).ok);
    assert(lifecycle.enable("root").ok);
    auto lock = lifecycle.lockfile().to_json();
    lock["packages"][0]["sourceUri"] = "file://tampered";
    write_file(store / "state" / "skills.lock", lock.dump(2) + "\n");
    bool rejected = false;
    try {
        auto replay_registry = std::make_shared<SkillRegistry>(base / "provenance-unused-2");
        SkillLifecycleManager replay(replay_registry, store);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    assert(rejected);
}

void test_failed_commit_retains_snapshot(const fs::path& base) {
    const fs::path sources = base / "failure-sources";
    const fs::path store = base / "failure-store";
    const auto old_package = make_package(sources, "old", "stable", "1.0.0", "old");
    const auto new_package = make_package(sources, "new", "stable", "1.1.0", "new");
    auto registry = std::make_shared<SkillRegistry>(base / "failure-unused");
    SkillLifecycleManager lifecycle(registry, store);
    assert(lifecycle.install(old_package).ok);
    assert(lifecycle.enable("stable", "^1.0.0").ok);
    const auto before = registry->snapshot();
    const auto before_lock = lifecycle.lockfile().dump();
    fs::remove(store / "state" / "history.json");
    fs::create_directory(store / "state" / "history.json");
    auto failed = lifecycle.update(new_package);
    assert(!failed.ok);
    assert(registry->snapshot().generation() == before.generation());
    assert(registry->snapshot().get("stable")->version == "1.0.0");
    assert(lifecycle.lockfile().dump() == before_lock);
}

} // namespace

int main() {
    const fs::path base = fs::temp_directory_path() / "agent_skill_lifecycle_stage5";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base);
    test_semver_and_resolution();
    test_lockfile_validation();
    test_atomic_registry_reload(base);
    test_lifecycle(base);
    test_failed_commit_retains_snapshot(base);
    test_lock_provenance_replay(base);
    fs::remove_all(base, ec);
    std::cout << "test_skill_lifecycle: ok\n";
    return 0;
}
