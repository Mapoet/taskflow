#include <agent/skills/skill_workflow.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <future>
#include <atomic>
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

SkillInvocationContext context_for(const fs::path& package) {
    SkillInvocationContext context;
    context.control = std::make_shared<TaskControl>();
    context.grants.tools = {"base_inc", "base_mul", "base_cancel",
                            "skill::workflow-skill::inc", "skill::workflow-skill::mul",
                            "skill::workflow-skill::cancel"};
    context.grants.filesystem_read = {package.string()};
    context.task_id = "trace-stage4";
    context.run_id = "run-stage4";
    return context;
}
} // namespace

int main() {
    const fs::path base = fs::temp_directory_path() / "agent_skill_workflow_stage4";
    std::error_code ec;
    fs::remove_all(base, ec);
    const fs::path package = base / "workflow-skill";
    write_file(base / "dep-skill/SKILL.md", R"(---
api-version: agent.taskflow/v1
kind: Skill
name: dep-skill
version: 2.1.0
description: Exact Stage 4 dependency
---
dependency
)");

    write_file(package / "tools/inc.json", R"({"source":"base_inc"})");
    write_file(package / "tools/mul.json", R"({"source":"base_mul"})");
    write_file(package / "tools/cancel.json", R"({"source":"base_cancel"})");
    write_file(package / "workflows/double.json", R"({
      "api-version":"agent.taskflow/workflow/v1","kind":"SkillWorkflow",
      "nodes":[{"id":"multiply","type":"tool","resource":"mul",
        "input":{"value":{"from":"$input","path":"/value"}}}],
      "outputs":{"value":{"from":"multiply","path":"/value"}}
    })");
    write_file(package / "workflows/main.json", R"({
      "api-version":"agent.taskflow/workflow/v1","kind":"SkillWorkflow",
      "nodes":[
        {"id":"seed","type":"tool","resource":"inc","idempotency-key":"seed-write",
         "input":{"value":{"from":"$input","path":"/value"}}},
        {"id":"repeat","type":"loop","max-iterations":10,
         "input":{"state":{"from":"seed"}},
         "body":{"type":"tool","resource":"inc","idempotency-key":"loop-write"},
         "condition":{"path":"/value","op":"gte","value":4}},
        {"id":"nested","type":"workflow","resource":"double",
         "input":{"value":{"from":"repeat","path":"/value"}}},
        {"id":"child","type":"child","backend":"local","idempotency-key":"child-write",
         "input":{"value":{"from":"nested","path":"/value"}}}
      ],
      "outputs":{"value":{"from":"child","path":"/value"}}
    })");
    write_file(package / "workflows/unkeyed.json", R"({
      "api-version":"agent.taskflow/workflow/v1","kind":"SkillWorkflow",
      "nodes":[{"id":"write","type":"tool","resource":"inc",
        "input":{"value":{"from":"$input","path":"/value"}}}],
      "outputs":{"value":{"from":"write","path":"/value"}}
    })");
    write_file(package / "workflows/escalate.json", R"({
      "api-version":"agent.taskflow/workflow/v1","kind":"SkillWorkflow",
      "nodes":[{"id":"child","type":"child","backend":"local",
        "idempotency-key":"denied-child",
        "permissions":{"tools":["admin"]},
        "input":{"value":{"from":"$input","path":"/value"}}}],
      "outputs":{"value":{"from":"child","path":"/value"}}
    })");
    write_file(package / "workflows/cancel.json", R"({
      "api-version":"agent.taskflow/workflow/v1","kind":"SkillWorkflow",
      "nodes":[{"id":"repeat","type":"loop","max-iterations":10,
        "input":{"state":{"from":"$input"}},
        "body":{"type":"tool","resource":"cancel","idempotency-key":"cancel-loop"},
        "condition":{"path":"/value","op":"gte","value":10}}],
      "outputs":{"value":{"from":"repeat","path":"/value"}}
    })");
    write_file(package / "SKILL.md", R"(---
api-version: agent.taskflow/v1
kind: Skill
name: workflow-skill
version: 1.0.0
description: Stage 4 workflow fixture
dependencies:
  - name: dep-skill
    version: 2.1.0
permissions:
  tools: [base_inc, base_mul, base_cancel, skill::workflow-skill::inc, skill::workflow-skill::mul, skill::workflow-skill::cancel]
  filesystem:
    read: [.]
resources:
  tools:
    - id: inc
      path: tools/inc.json
    - id: mul
      path: tools/mul.json
    - id: cancel
      path: tools/cancel.json
  workflows:
    - id: double
      path: workflows/double.json
    - id: main
      path: workflows/main.json
    - id: unkeyed
      path: workflows/unkeyed.json
    - id: escalate
      path: workflows/escalate.json
    - id: cancel-flow
      path: workflows/cancel.json
---
stage 4
)");

    auto registry = std::make_shared<SkillRegistry>(base);
    registry->scan_or_reload();
    if (!registry->valid()) {
        for (const auto& diagnostic : registry->diagnostics())
            std::cerr << diagnostic.message << '\n';
    }
    assert(registry->valid());
    auto loader = std::make_shared<SkillLoader>(*registry);
    auto skill_runtime = std::make_shared<SkillRuntime>(registry, loader);
    auto bus = std::make_shared<ToolBus>();
    int increments = 0;
    int multiplies = 0;
    std::atomic_bool block_increment{false};
    std::atomic_bool increment_entered{false};
    std::atomic_bool release_increment{false};
    ToolMeta write_meta;
    write_meta.side_effect = ToolSideEffect::Write;
    write_meta.schema = {{"type", "object"},
                         {"properties", {{"value", {{"type", "integer"}}}}},
                         {"additionalProperties", false}};
    bus->register_local_tool("base_inc", [&](const json& input) {
        ++increments;
        if (block_increment.load()) {
            increment_entered.store(true);
            while (!release_increment.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return json{{"value", input.at("value").get<int>() + 1}};
    }, write_meta);
    ToolMeta read_meta;
    read_meta.side_effect = ToolSideEffect::ReadOnly;
    read_meta.schema = write_meta.schema;
    bus->register_local_tool("base_mul", [&](const json& input) {
        ++multiplies;
        return json{{"value", input.at("value").get<int>() * 2}};
    }, read_meta);
    auto capabilities = std::make_shared<SkillCapabilityRuntime>(
        registry, loader, skill_runtime, bus);
    SkillWorkflowRuntime workflows(registry, loader, skill_runtime, capabilities);

    int child_calls = 0;
    auto child = std::make_shared<LocalChildTaskBackend>(
        [&](const ChildTaskRequest& request) {
            ++child_calls;
            ChildTaskResult result;
            result.status = ChildTaskStatus::Completed;
            result.child_id = request.child_id;
            result.run_id = request.run_id;
            result.attempt = request.attempt;
            result.outputs = {{"value", request.inputs.at("value").get<int>() + 1}};
            result.checkpoint = {{"complete", true}};
            return result;
        });
    SkillWorkflowRunOptions options;
    options.context = context_for(package);
    options.child_backend = [child](const std::string& name) {
        return name == "local" ? std::static_pointer_cast<ChildTaskBackend>(child) : nullptr;
    };

    int cancel_calls = 0;
    std::shared_ptr<TaskControl> active_cancel_control;
    bus->register_local_tool("base_cancel", [&](const json& input) {
        ++cancel_calls;
        if (active_cancel_control) active_cancel_control->request_cancel();
        return json{{"value", input.at("value").get<int>() + 1}};
    }, write_meta);

    auto result = workflows.run("workflow-skill", "main", {{"value", 0}}, options);
    if (!result.ok) std::cerr << result.error.dump(2) << '\n';
    assert(result.ok);
    assert(result.output.at("value") == 9);
    assert(result.dependency_lock.at("workflow-skill") == "1.0.0");
    assert(result.dependency_lock.at("dep-skill") == "2.1.0");
    assert(increments == 4 && multiplies == 1 && child_calls == 1);
    assert(result.checkpoint.at("iterations").begin().value() == 3);

    SkillWorkflowRunOptions resume = options;
    resume.mode = SkillWorkflowStartMode::Resume;
    resume.checkpoint = result.checkpoint;
    resume.context = context_for(package);
    auto resumed = workflows.run("workflow-skill", "main", {{"value", 0}}, resume);
    assert(resumed.ok && resumed.output == result.output);
    assert(increments == 4 && multiplies == 1 && child_calls == 1);

    SkillWorkflowRunOptions restart = options;
    restart.mode = SkillWorkflowStartMode::Restart;
    restart.checkpoint = result.checkpoint;
    restart.context = context_for(package);
    auto restarted = workflows.run("workflow-skill", "main", {{"value", 0}}, restart);
    assert(restarted.ok && restarted.output == result.output);
    assert(increments == 4 && multiplies == 2 && child_calls == 1);

    // Resume from an iteration boundary, not from the start of the loop.
    json partial = result.checkpoint;
    const std::string root_path = "workflow-skill/main";
    partial["completed"].erase(root_path + "/repeat");
    partial["completed"].erase(root_path + "/nested");
    partial["completed"].erase(root_path + "/child");
    partial["iterations"][root_path + "/repeat"] = 2;
    partial["loopState"][root_path + "/repeat"] = {{"value", 3}};
    partial["idempotency"].erase("loop-write/2");
    SkillWorkflowRunOptions partial_resume = options;
    partial_resume.mode = SkillWorkflowStartMode::Resume;
    partial_resume.checkpoint = partial;
    partial_resume.context = context_for(package);
    auto continued = workflows.run("workflow-skill", "main", {{"value", 0}}, partial_resume);
    assert(continued.ok && continued.output == result.output);
    assert(increments == 5 && multiplies == 2 && child_calls == 1);

    // Restart refuses an unkeyed write because replay cannot be proven safe.
    SkillWorkflowRunOptions unkeyed_options = options;
    unkeyed_options.context = context_for(package);
    auto unkeyed = workflows.run("workflow-skill", "unkeyed", {{"value", 0}}, unkeyed_options);
    assert(unkeyed.ok);
    SkillWorkflowRunOptions unkeyed_restart = unkeyed_options;
    unkeyed_restart.mode = SkillWorkflowStartMode::Restart;
    unkeyed_restart.checkpoint = unkeyed.checkpoint;
    unkeyed_restart.context = context_for(package);
    auto replay_denied = workflows.run(
        "workflow-skill", "unkeyed", {{"value", 0}}, unkeyed_restart);
    assert(!replay_denied.ok);
    assert(replay_denied.error.value("code", "") == kSkillWorkflowReplayDenied);

    // A child cannot widen the parent task's grants.
    SkillWorkflowRunOptions escalation_options = options;
    escalation_options.context = context_for(package);
    auto escalation = workflows.run(
        "workflow-skill", "escalate", {{"value", 1}}, escalation_options);
    assert(!escalation.ok);
    assert(escalation.error.value("code", "") == kSkillPermissionDenied);
    assert(child_calls == 1);

    SkillWorkflowRunOptions cancelled_options = options;
    cancelled_options.context = context_for(package);
    active_cancel_control = cancelled_options.context.control;
    auto cancelled = workflows.run(
        "workflow-skill", "cancel-flow", {{"value", 0}}, cancelled_options);
    active_cancel_control.reset();
    assert(!cancelled.ok);
    if (cancelled.error.value("code", "") != kSkillCancelled)
        std::cerr << cancelled.error.dump(2) << '\n';
    assert(cancelled.error.value("code", "") == kSkillCancelled);
    assert(cancel_calls == 1);
    assert(cancelled.checkpoint.at("completed").empty());

    // Workflow descriptors are preloaded; an in-flight file update affects only later runs.
    block_increment.store(true);
    SkillWorkflowRunOptions snapshot_options = options;
    snapshot_options.context = context_for(package);
    auto snapshot_run = std::async(std::launch::async, [&] {
        return workflows.run("workflow-skill", "main", {{"value", 0}}, snapshot_options);
    });
    while (!increment_entered.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    write_file(package / "workflows/double.json", R"({
      "api-version":"agent.taskflow/workflow/v1","kind":"SkillWorkflow",
      "nodes":[{"id":"changed","type":"tool","resource":"inc",
        "input":{"value":{"from":"$input","path":"/value"}}}],
      "outputs":{"value":{"from":"changed","path":"/value"}}
    })");
    release_increment.store(true);
    auto pinned = snapshot_run.get();
    assert(pinned.ok && pinned.output.at("value") == 9);
    block_increment.store(false);

    json malformed = {
        {"api-version", "agent.taskflow/workflow/v1"}, {"kind", "SkillWorkflow"},
        {"nodes", json::array({{{"id", "write"}, {"type", "tool"},
          {"resource", "inc"}, {"input", {{"value", {{"from", "future"}}}}}}})},
        {"outputs", {{"value", {{"from", "write"}, {"path", "/value"}}}}}
    };
    auto invalid = validate_skill_workflow_descriptor(malformed);
    assert(!invalid.ok);
    assert(invalid.error.value("code", "") == kSkillWorkflowMappingInvalid);

    fs::remove_all(base, ec);
    std::cout << "test_skill_workflow: ok\n";
    return 0;
}
