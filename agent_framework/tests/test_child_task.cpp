#include <agent/child_task.hpp>

#include <cassert>
#include <iostream>

using namespace agent_framework;

int main() {
    LocalChildTaskBackend backend([](const ChildTaskRequest& request) {
        ChildTaskResult result;
        result.status = ChildTaskStatus::Completed;
        result.child_id = request.child_id;
        result.run_id = request.run_id;
        result.attempt = request.attempt;
        result.outputs = {{"mode", static_cast<int>(request.mode)}, {"value", request.inputs.at("value")}};
        result.usage.iterations = 2;
        return result;
    });

    ChildTaskRequest request;
    request.child_id = "child-1";
    request.parent_run_id = "parent";
    request.run_id = "run-2";
    request.trace_id = "trace";
    request.depth = 2;
    request.attempt = 1;
    request.mode = ChildTaskStartMode::Resume;
    request.inputs = {{"value", 42}};
    request.checkpoint = {{"node", 1}};
    request.grants.tools = {"read"};
    const json metadata = child_task_request_metadata(request);
    assert(metadata.at("startMode") == "resume");
    assert(metadata.at("checkpoint") == request.checkpoint);
    assert(metadata.at("permissions").at("tools").at(0) == "read");
    auto result = backend.start(request)->wait();
    assert(result.ok());
    assert(result.outputs.at("value") == 42);
    assert(result.usage.iterations == 2);
    result.error_code = "none";
    result.events = json::array({{{"type", "completed"}}});
    result.checkpoint = {{"node", 2}};
    const auto normalized = child_task_result_from_json(child_task_result_to_json(result));
    assert(normalized.status == result.status);
    assert(normalized.outputs == result.outputs);
    assert(normalized.checkpoint == result.checkpoint);

    SkillPermissionGrant parent;
    parent.tools = {"read", "write"};
    parent.secrets = {"token"};
    SkillPermissionGrant narrower;
    narrower.tools = {"read"};
    assert(child_task_grants_are_narrower(parent, narrower));
    narrower.tools.push_back("admin");
    std::string escalation;
    assert(!child_task_grants_are_narrower(parent, narrower, &escalation));
    assert(escalation.find("admin") != std::string::npos);

    request.policy.cancel_requested = std::make_shared<std::atomic_bool>(false);
    request.policy.max_output_bytes = 1;
    result = backend.start(request)->wait();
    assert(result.status == ChildTaskStatus::Failed);
    assert(result.error_code == "child_task_output_budget_exceeded");

    request.depth = request.policy.max_depth + 1;
    bool rejected = false;
    try {
        (void)backend.start(request);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);

    request.depth = 0;
    request.policy.cancel_requested = std::make_shared<std::atomic_bool>(true);
    result = backend.start(request)->wait();
    assert(result.status == ChildTaskStatus::Cancelled);

    std::cout << "test_child_task: ok\n";
}
