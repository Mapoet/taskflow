#include <agent/skill_lifecycle.hpp>
#include <agent/skill_resource_access.hpp>
#include <agent/skill_resource_cache.hpp>
#include <agent/skill_runtime.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace agent_framework;

void write(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << value;
    assert(output.good());
}
}

int main() {
    const auto root = fs::temp_directory_path() / "taskflow-skill-audit";
    std::error_code ec;
    fs::remove_all(root, ec);
    const auto package = root / "audit";
    write(package / "data.json", "{\"value\":7}");
    write(package / "SKILL.md", R"(---
api-version: agent.taskflow/v1
kind: Skill
name: audit
version: 1.2.3
description: audit fixture
resources:
  tools:
    - id: local
      path: data.json
  assets:
    - id: data
      path: data.json
      media-type: application/json
      read-mode: text
      sha256: 9d709bba0358da8fdf5600bb3692fa32d0aa2a19a08152cf4307e0b4240e71ac
      size: 11
      license: Apache-2.0
      source: package://data.json
      cache-policy: on-demand
---
body
)");
    auto registry = std::make_shared<SkillRegistry>(root);
    registry->scan_or_reload();
    assert(registry->valid());
    auto entry = registry->get("audit");
    auto manifest = registry->get_manifest("audit");
    assert(entry && manifest);

    std::vector<SkillAuditRecord> records;
    SkillInvocationContext context;
    context.task_id = "task";
    context.session_id = "session";
    context.trace_id = "trace";
    context.run_id = "run";
    context.attempt = 2;
    context.iteration = 3;
    context.depth = 4;
    context.audit_sink = [&](const SkillAuditRecord& record) { records.push_back(record); };
    auto loader = std::make_shared<SkillLoader>(*registry);
    SkillRuntime runtime(registry, loader);
    auto begun = runtime.begin("audit", "local", SkillResourceType::Tool,
                               nlohmann::json::object(), context);
    assert(begun.ok && begun.ticket);
    assert(begun.ticket->context.skill_id == "audit");
    assert(begun.ticket->context.skill_version == "1.2.3");
    assert(begun.ticket->context.registry_generation == registry->snapshot().generation());
    assert(runtime.finish(*begun.ticket, nlohmann::json::object()).ok);
    assert(records.size() == 2U);
    assert(records.front().identity.trace_id == "trace");
    assert(records.front().identity.depth == 4);

    auto cache = std::make_shared<SkillResourceCache>(root / "cache");
    SkillResourceAccess access(cache);
    SkillResourceOpenOptions open_options;
    open_options.mode = SkillResourceReadMode::Text;
    open_options.max_bytes = 11;
    open_options.audit_sink = context.audit_sink;
    open_options.audit_identity = records.front().identity;
    auto opened = access.open_snapshot(*entry, manifest, "data", open_options);
    assert(opened.ok && opened.handle && opened.handle->cache_lease);
    assert(records.size() == 4U);
    assert(records[2].category == "cache" && records[3].category == "resource");
    for(const auto& record : records)
        assert(record.to_json().dump().find("secret-value") == std::string::npos);

    SkillInvocationContext non_interfering = context;
    non_interfering.audit_sink = [](const SkillAuditRecord&) { throw std::runtime_error("sink"); };
    assert(runtime.begin("audit", "local", SkillResourceType::Tool,
                         nlohmann::json::object(), non_interfering).ok);

    fs::remove_all(root, ec);
    std::cout << "skill audit tests passed\n";
}
