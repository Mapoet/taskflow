#include <agent/skills/skill_runtime.hpp>
#include <agent/skills/skill_script_tool.hpp>
#include <agent/skills/skill_services.hpp>
#include <agent/toolbus/toolbus.hpp>

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>

namespace {
namespace fs = std::filesystem;
using namespace agent_framework;
using json = nlohmann::json;

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    assert(output.good());
    output << content;
}

SkillPermissionGrant grants(const fs::path& package) {
    SkillPermissionGrant result;
    result.tools = {"run_skill_script", "run_skill_cli"};
    result.environment = {"SAFE_VALUE"};
    result.filesystem_read = {package.string()};
    result.filesystem_write = {(package / "output").string()};
    result.secrets = {"api-token"};
    return result;
}

ToolCallControl control_for(const fs::path& package, std::vector<SkillEvent>& events) {
    SkillInvocationContext context;
    context.control = std::make_shared<TaskControl>();
    const auto task_control = context.control;
    context.grants = grants(package);
    context.environment["SAFE_VALUE"] = "visible";
    context.environment["HOST_ONLY"] = "must-not-leak";
    context.secret_provider = [](std::string_view reference) -> std::optional<std::string> {
        if (reference == "api-token") return "stage2-secret-value";
        return std::nullopt;
    };
    context.event_sink = [&](const SkillEvent& event) { events.push_back(event); };
    context.task_id = "sandbox-task";
    context.run_id = "sandbox-run";
    ToolCallControl control;
    control.cancellation_requested = [task_control] {
        return task_control->is_cancel_requested();
    };
    control.skill_context = std::make_shared<SkillInvocationContext>(std::move(context));
    control.active_skill_id = "sandbox-skill";
    return control;
}

} // namespace

int main() {
#if defined(_WIN32)
    std::clog << "test_skill_process_sandbox: skip (Linux sandbox required)\n";
    return 0;
#else
    const fs::path base = fs::temp_directory_path() / "agent_skill_process_sandbox_test";
    std::error_code ec;
    fs::remove_all(base, ec);
    const fs::path package = base / "sandbox-skill";
    fs::create_directories(package / "output");
    write_file(package / "schemas/input.json",
               R"({"type":"object","properties":{"value":{"type":"integer"}},"required":["value"]})");
    write_file(package / "schemas/output.json",
               R"({"type":"object","properties":{"ok":{"type":"boolean"},"secret":{"type":"string"}},"required":["ok","secret"]})");
    write_file(package / "scripts/probe.sh", R"SH(#!/bin/sh
set -eu
input="$(cat)"
test "$SAFE_VALUE" = visible
test -z "${HOST_ONLY-}"
test ! -e /home/Mapoet/.ssh
test -f "$AGENT_SECRET_API_TOKEN_FILE"
secret="$(cat "$AGENT_SECRET_API_TOKEN_FILE")"
printf '%s' "$input" > output/input.json
if curl --max-time 1 -sS https://example.com >/dev/null 2>&1; then exit 41; fi
printf '{"ok":true,"secret":"%s"}\n' "$secret"
)SH");
    write_file(package / "cli/echo", R"SH(#!/bin/sh
printf 'cli:%s:%s\n' "$1" "${HOST_ONLY-unset}"
)SH");
    write_file(package / "scripts/wait.sh", R"SH(#!/bin/sh
sleep 10
)SH");
    write_file(package / "scripts/busy.sh", R"SH(#!/bin/sh
while :; do :; done
)SH");
    write_file(package / "scripts/memory.py", R"PY(import json
try:
    bytearray(512 * 1024 * 1024)
except MemoryError:
    print(json.dumps({"memory_limited": True}))
    raise SystemExit(0)
raise SystemExit(44)
)PY");
    fs::permissions(package / "cli/echo",
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec);
    write_file(package / "SKILL.md", R"(---
api-version: agent.taskflow/v1
kind: Skill
name: sandbox-skill
version: 1.0.0
description: Stage 2 process sandbox fixture
permissions:
  tools: [run_skill_script, run_skill_cli]
  env: [SAFE_VALUE]
  filesystem:
    read: [.]
    write: [output]
  secrets: [api-token]
resources:
  schemas:
    - id: input
      path: schemas/input.json
    - id: output
      path: schemas/output.json
  scripts:
    - id: probe
      path: scripts/probe.sh
      executable: true
      input-schema: input
      output-schema: output
    - id: wait
      path: scripts/wait.sh
      executable: true
    - id: busy
      path: scripts/busy.sh
      executable: true
    - id: memory
      path: scripts/memory.py
      executable: true
  cli:
    - id: echo
      path: cli/echo
      executable: true
---
body
)");

    auto registry = std::make_shared<SkillRegistry>(base);
    registry->scan_or_reload();
    assert(registry->valid());
    auto services = std::make_shared<SkillServices>();
    services->registry = registry;
    services->loader = std::make_shared<SkillLoader>(*registry);
    services->runtime = std::make_shared<SkillRuntime>(registry, services->loader);
    ToolBus bus;
    register_skill_script_tool(bus, services);

    const json request{{"skill_id", "sandbox-skill"}, {"relative_path", "scripts/probe.sh"},
                       {"input", {{"value", 7}}}};
    const json missing_read_context = bus.call_tool(
        "read_skill_resource",
        {{"skill_id", "sandbox-skill"}, {"relative_path", "scripts/probe.sh"},
         {"kind", "script"}}).get();
    assert(missing_read_context.value("code", "") == kSkillPermissionDenied);
    const json missing_context = bus.call_tool("run_skill_script", request).get();
    assert(missing_context.value("code", "") == kSkillPermissionDenied);

    std::vector<SkillEvent> events;
    ToolCallControl control = control_for(package, events);
    const json declared_read = bus.call_tool(
        "read_skill_resource",
        {{"skill_id", "sandbox-skill"}, {"relative_path", "scripts/probe.sh"},
         {"kind", "script"}}, control).get();
    assert(declared_read.value("content", "").find("#!/bin/sh") == 0U);
    const json result = bus.call_tool("run_skill_script", request, control).get();
    assert(result.value("exit_code", -1) == 0);
    assert(result["output"]["ok"] == true);
    assert(result["output"]["secret"] == "[REDACTED]");
    assert(result.dump().find("stage2-secret-value") == std::string::npos);
    assert(result.value("stderr", "").empty());
    assert(fs::exists(package / "output/input.json"));
    assert(json::parse(std::ifstream(package / "output/input.json"))["value"] == 7);
    assert(events.size() == 2U);
    assert(events.front().type == SkillEventType::InvocationStarted);
    assert(events.back().type == SkillEventType::InvocationCompleted);

    const json cli = bus.call_tool(
        "run_skill_cli",
        {{"skill_id", "sandbox-skill"}, {"relative_path", "cli/echo"},
         {"args", json::array({"ok"})}}, control).get();
    assert(cli.value("exit_code", -1) == 0);
    assert(cli.value("stdout", "") == "cli:ok:unset\n");

    std::vector<SkillEvent> cancellation_events;
    ToolCallControl cancellation_control = control_for(package, cancellation_events);
    auto cancelled_result = bus.call_tool(
        "run_skill_script",
        {{"skill_id", "sandbox-skill"}, {"relative_path", "scripts/wait.sh"}},
        cancellation_control);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    cancellation_control.skill_context->control->request_cancel();
    const json cancelled = cancelled_result.get();
    assert(cancelled.value("code", "") == kSkillCancelled);
    assert(cancelled.value("cancelled", false));
    assert(cancellation_events.size() == 2U);
    assert(cancellation_events.front().type == SkillEventType::InvocationStarted);
    assert(cancellation_events.back().type == SkillEventType::Cancelled);

    std::vector<SkillEvent> cpu_events;
    ToolCallControl cpu_control = control_for(package, cpu_events);
    auto cpu_context = *cpu_control.skill_context;
    cpu_context.limits.max_cpu_time = std::chrono::milliseconds(200);
    cpu_control.skill_context = std::make_shared<SkillInvocationContext>(std::move(cpu_context));
    const json cpu_limited = bus.call_tool(
        "run_skill_script",
        {{"skill_id", "sandbox-skill"}, {"relative_path", "scripts/busy.sh"}},
        cpu_control).get();
    assert(cpu_limited.value("code", "") == kSkillResourceBudgetExceeded);
    assert(cpu_limited.value("budget_exceeded", "") == "cpu");
    assert(cpu_events.back().type == SkillEventType::BudgetExceeded);

    std::vector<SkillEvent> memory_events;
    ToolCallControl memory_control = control_for(package, memory_events);
    auto memory_context = *memory_control.skill_context;
    memory_context.limits.max_memory_bytes = 128U * 1024U * 1024U;
    memory_control.skill_context = std::make_shared<SkillInvocationContext>(std::move(memory_context));
    const json memory_limited = bus.call_tool(
        "run_skill_script",
        {{"skill_id", "sandbox-skill"}, {"relative_path", "scripts/memory.py"}},
        memory_control).get();
    assert(memory_limited.value("exit_code", -1) == 0);
    assert(memory_limited.value("stdout", "").find("memory_limited") != std::string::npos);

    auto denied_context = *control.skill_context;
    denied_context.grants.filesystem_write.clear();
    ToolCallControl denied_control = control;
    denied_control.skill_context =
        std::make_shared<SkillInvocationContext>(std::move(denied_context));
    fs::remove(package / "output/input.json", ec);
    const json denied_write = bus.call_tool("run_skill_script", request, denied_control).get();
    assert(!denied_write.contains("exit_code") || denied_write.at("exit_code").get<int>() != 0);
    assert(!fs::exists(package / "output/input.json"));

    fs::remove_all(base, ec);
    std::cout << "test_skill_process_sandbox: ok\n";
    return 0;
#endif
}
