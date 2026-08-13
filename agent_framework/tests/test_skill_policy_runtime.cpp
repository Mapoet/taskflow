#include <agent/toolbus/schema_validate.hpp>
#include <agent/skills/skill_runtime.hpp>
#include <agent/toolbus/toolbus.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
namespace fs = std::filesystem;
using namespace agent_framework;

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    assert(output.good());
    output << content;
}

SkillPermissionSet requested_permissions() {
    SkillPermissionSet permissions;
    permissions.tools = {"allowed"};
    permissions.network = {"https://api.example.org", "https://*.data.example.org"};
    permissions.environment = {"DATA_HOME"};
    permissions.filesystem_read = {"data"};
    permissions.filesystem_write = {"output"};
    permissions.secrets = {"api-token"};
    return permissions;
}

SkillPermissionGrant grants(const fs::path& package) {
    SkillPermissionGrant grant;
    grant.tools = {"allowed", "not-requested"};
    grant.network = {"https://api.example.org", "https://*.data.example.org"};
    grant.environment = {"DATA_HOME", "OTHER"};
    grant.filesystem_read = {(package / "data").string()};
    grant.filesystem_write = {(package / "output").string()};
    grant.secrets = {"api-token", "other-token"};
    return grant;
}

void test_policy(const fs::path& package) {
    SkillPolicyEngine policy(requested_permissions(), grants(package), package);
    assert(policy.authorize_tool("allowed").allowed);
    assert(!policy.authorize_tool("not-requested").allowed);
    assert(policy.authorize_network("https://api.example.org/v1?q=1").allowed);
    assert(policy.authorize_network("https://x.data.example.org/path").allowed);
    assert(!policy.authorize_network("https://data.example.org").allowed);
    assert(!policy.authorize_network("https://evil.example.org").allowed);
    assert(policy.authorize_environment("DATA_HOME").allowed);
    assert(!policy.authorize_environment("OTHER").allowed);
    assert(policy.authorize_filesystem(package / "data" / "input.json", false).allowed);
    assert(!policy.authorize_filesystem(package / "data" / "input.json", true).allowed);
    assert(policy.authorize_filesystem(package / "output" / "result.json", true).allowed);
    assert(!policy.authorize_filesystem(package.parent_path() / "outside", false).allowed);
    assert(policy.authorize_secret("api-token").allowed);
    assert(!policy.authorize_secret("other-token").allowed);
}

void test_portable_tool_aliases() {
    SkillPermissionSet requested;
    requested.tools = {"Read", "Python"};
    SkillPermissionGrant granted;
    granted.tools = {"fs_read", "python3"};
    SkillPolicyEngine policy(requested, granted);
    assert(policy.authorize_tool("Read").allowed);
    assert(policy.authorize_tool("fs_read").allowed);
    assert(policy.authorize_tool("Python").allowed);
    assert(policy.authorize_tool("python3").allowed);
    assert(!policy.authorize_tool("Write").allowed);
    assert(policy.authorize_tool("fs_read").target == "Read");
}

void test_generic_schema_paths() {
    const auto schema = nlohmann::json::parse(R"({
      "type":"object",
      "properties":{"rows":{"type":"array","minItems":1,"items":{"type":"object","properties":{"value":{"type":"integer","minimum":0}},"required":["value"]}}},
      "required":["rows"]
    })");
    nlohmann::json error;
    assert(!validate_json_instance(schema, {{"rows", nlohmann::json::array({{{"value", -1}}})}}, error));
    assert(error["details"]["instance_path"] == "/rows/0/value");
    assert(error["details"]["schema_path"] == "/properties/rows/items/properties/value/minimum");
    assert(error["details"]["keyword"] == "minimum");
}

void test_runtime(const fs::path& base, const fs::path& package) {
    write_file(package / "schemas/input.json",
               R"({"type":"object","properties":{"value":{"type":"integer","minimum":0}},"required":["value"]})");
    write_file(package / "schemas/output.json",
               R"({"type":"object","properties":{"ok":{"type":"boolean"}},"required":["ok"]})");
    write_file(package / "scripts/run.sh", "#!/bin/sh\n");
    write_file(package / "cli/run", "#!/bin/sh\n");
    write_file(package / "mcp/server.json", "{}\n");
    write_file(package / "workflows/run.json", "{}\n");
    write_file(package / "SKILL.md", R"(---
api-version: agent.taskflow/v1
kind: Skill
name: policy-skill
version: 1.0.0
description: policy fixture
permissions:
  tools: [allowed]
  network: [https://api.example.org]
  env: [DATA_HOME]
  filesystem:
    read: [data]
    write: [output]
  secrets: [api-token]
resources:
  schemas:
    - id: input
      path: schemas/input.json
    - id: output
      path: schemas/output.json
  scripts:
    - id: run
      path: scripts/run.sh
      input-schema: input
      output-schema: output
  cli:
    - id: cli-run
      path: cli/run
      executable: true
      input-schema: input
      output-schema: output
  mcp:
    - id: mcp-run
      path: mcp/server.json
      input-schema: input
      output-schema: output
  workflows:
    - id: workflow-run
      path: workflows/run.json
      input-schema: input
      output-schema: output
---
body
)");
    auto registry = std::make_shared<SkillRegistry>(base);
    registry->scan_or_reload();
    assert(registry->valid());
    auto loader = std::make_shared<SkillLoader>(*registry);
    SkillRuntime runtime(registry, loader);
    std::vector<SkillEvent> events;
    SkillInvocationContext context;
    context.grants = grants(package);
    context.task_id = "task-1";
    context.run_id = "run-1";
    context.event_sink = [&](const SkillEvent& event) { events.push_back(event); };
    auto begun = runtime.begin("policy-skill", "run", SkillResourceType::Script,
                               {{"value", 1}}, context);
    assert(begun.ok && begun.ticket);
    assert(begun.ticket->policy->authorize_tool("allowed").allowed);
    auto finished = runtime.finish(*begun.ticket, {{"ok", true}});
    assert(finished.ok);
    assert(events.size() == 2U);
    assert(events.front().type == SkillEventType::InvocationStarted);
    assert(events.back().type == SkillEventType::InvocationCompleted);

    const std::vector<std::pair<std::string, SkillResourceType>> guarded_resources = {
        {"cli-run", SkillResourceType::Cli},
        {"mcp-run", SkillResourceType::Mcp},
        {"workflow-run", SkillResourceType::Workflow}};
    for (const auto& [resource_id, kind] : guarded_resources) {
        auto guarded = runtime.begin("policy-skill", resource_id, kind, {{"value", 1}}, context);
        assert(guarded.ok && guarded.ticket);
        assert(guarded.ticket->context.control == context.control);
        assert(guarded.ticket->context.event_sink);
        assert(guarded.ticket->policy->authorize_tool("allowed").allowed);
        assert(runtime.finish(*guarded.ticket, {{"ok", true}}).ok);
    }

    auto bad_input = runtime.begin("policy-skill", "run", SkillResourceType::Script,
                                   {{"value", -2}}, context);
    assert(!bad_input.ok && bad_input.error["code"] == kSkillInputInvalid);
    assert(bad_input.error["details"]["instance_path"] == "/value");
    assert(bad_input.error["details"].contains("schema_location"));
    auto bad_output = runtime.finish(*begun.ticket, {{"ok", "yes"}});
    assert(!bad_output.ok && bad_output.error["code"] == kSkillOutputInvalid);

    context.limits.max_input_bytes = 2;
    auto over_budget = runtime.begin("policy-skill", "run", SkillResourceType::Script,
                                     {{"value", 1}}, context);
    assert(!over_budget.ok && over_budget.error["code"] == kSkillResourceBudgetExceeded);

    context.limits.max_input_bytes = 1024;
    context.control = std::make_shared<TaskControl>();
    context.control->request_cancel();
    auto cancelled = runtime.begin("policy-skill", "run", SkillResourceType::Script,
                                   {{"value", 1}}, context);
    assert(!cancelled.ok && cancelled.error["code"] == kSkillCancelled);

    context.control = std::make_shared<TaskControl>();
    context.control->mark_deadline_exceeded();
    auto timed_out = runtime.begin("policy-skill", "run", SkillResourceType::Script,
                                   {{"value", 1}}, context);
    assert(!timed_out.ok && timed_out.error["code"] == kSkillCancelled);
    assert(events.back().type == SkillEventType::TimedOut);

    for (const auto& event : events) {
        assert(event.details.dump().find("api-token") == std::string::npos);
    }
}

void test_toolbus_request_authorization() {
    ToolBus bus;
    ToolMeta meta;
    meta.name = "allowed";
    meta.schema = {{"type", "object"},
                   {"properties", {{"target", {{"type", "string"}}}}},
                   {"required", {"target"}}};
    int calls = 0;
    bus.register_local_tool("allowed", [&](const nlohmann::json& input) {
        ++calls;
        return input;
    }, meta);
    ToolCallControl control;
    int authorization_calls = 0;
    control.authorization = [&](const std::string&, const nlohmann::json& arguments,
                                const ToolMeta&) -> std::optional<nlohmann::json> {
        ++authorization_calls;
        if (arguments.at("target") == "denied") {
            return nlohmann::json{{"error", "denied"}, {"code", kSkillPermissionDenied}};
        }
        return std::nullopt;
    };
    bus.add_tool_call_hook([](const std::string&, const nlohmann::json&) {
        ToolHookResult result;
        result.verdict = ToolHookVerdict::Replace;
        result.replaced_arguments = nlohmann::json{{"target", "denied"}};
        return result;
    });
    auto denied = bus.call_tool("allowed", {{"target", "allowed"}}, control).get();
    assert(denied["code"] == kSkillPermissionDenied);
    assert(authorization_calls == 2);
    assert(calls == 0);
    assert(bus.export_as_llm_tools([](std::string_view) { return false; }).empty());
}

} // namespace

int main() {
    const fs::path base = fs::temp_directory_path() / "agent_skill_policy_runtime_test";
    std::error_code ec;
    fs::remove_all(base, ec);
    const fs::path package = base / "policy";
    fs::create_directories(package / "data");
    fs::create_directories(package / "output");
    test_policy(package);
    test_portable_tool_aliases();
    test_generic_schema_paths();
    test_runtime(base, package);
    test_toolbus_request_authorization();
    fs::remove_all(base, ec);
    std::cout << "test_skill_policy_runtime: ok\n";
    return 0;
}
