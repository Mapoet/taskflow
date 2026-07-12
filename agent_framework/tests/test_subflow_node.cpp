#include <node/subflow_node.hpp>

#include <cassert>
#include <stdexcept>
#include <string>

int main() {
    using namespace agent_framework::node;
    tf::Executor executor(4);
    workflow::GraphBuilder builder("agent_subflows");
    builder.create_any_source("input", {{"value", 5}});

    SubflowNode::create(
        builder, "success", {{"input", "value"}},
        [](const SubflowRequest& request) {
            SubflowResult result;
            result.ok = true;
            result.outputs["value"] = std::any_cast<int>(request.inputs.at("value")) + 1;
            result.usage.iterations = 1;
            result.usage.tool_calls = 1;
            return result;
        }, {}, 0, SubflowStartMode::Start, "success_result");
    SubflowNode::create(
        builder, "failure", {{"input", "value"}},
        [](const SubflowRequest&) -> SubflowResult {
            throw std::runtime_error("child failed");
        }, {}, 0, SubflowStartMode::Start, "failure_result");

    bool sink_called = false;
    builder.create_any_sink(
        "aggregate",
        {{"success", "success_result"}, {"failure", "failure_result"}},
        [&sink_called](const workflow::ValueMap& values) {
            const auto success = std::any_cast<SubflowResult>(values.at("success_result"));
            const auto failure = std::any_cast<SubflowResult>(values.at("failure_result"));
            assert(success.ok);
            assert(!failure.ok);
            sink_called = true;
        });
    builder.run(executor);
    assert(sink_called);

    const auto good = std::any_cast<SubflowResult>(builder.get_latest_output("success", "success_result"));
    const auto bad = std::any_cast<SubflowResult>(builder.get_latest_output("failure", "failure_result"));
    assert(good.ok);
    assert(std::any_cast<int>(good.outputs.at("value")) == 6);
    assert(!good.run_id.empty());
    assert(!bad.ok);
    assert(bad.error == "child failed");
    assert(std::any_cast<int>(good.outputs.at("value")) == 6);

    workflow::GraphBuilder bounded("bounded_subflow");
    bounded.create_any_source("input", {{"value", 1}});
    SubflowLimits limits;
    limits.max_iterations = 0;
    SubflowNode::create(
        bounded, "bounded", {{"input", "value"}},
        [](const SubflowRequest&) {
            SubflowResult result;
            result.ok = true;
            return result;
        },
        limits);
    bounded.run(executor);
    const auto rejected = std::any_cast<SubflowResult>(
        bounded.get_latest_output("bounded", "subflow_result"));
    assert(!rejected.ok);
    assert(rejected.error == "budget_exceeded");

    workflow::GraphBuilder lifecycle("subflow_lifecycle");
    lifecycle.create_any_source("input", {{"value", 2}});
    SubflowNode::create(
        lifecycle, "retry", {{"input", "value"}},
        [](const SubflowRequest& request) {
            SubflowResult result;
            result.ok = request.attempt == 3 && request.mode == SubflowStartMode::Retry;
            return result;
        }, {}, 3, SubflowStartMode::Retry);
    SubflowNode::create(
        lifecycle, "restart", {{"input", "value"}},
        [](const SubflowRequest& request) {
            SubflowResult result;
            result.ok = request.attempt == 4 && request.mode == SubflowStartMode::Restart;
            return result;
        }, {}, 4, SubflowStartMode::Restart);
    SubflowNode::create(
        lifecycle, "resume", {{"input", "value"}},
        [](const SubflowRequest& request) {
            SubflowResult result;
            result.ok = request.attempt == 4 && request.mode == SubflowStartMode::Resume;
            return result;
        }, {}, 4, SubflowStartMode::Resume);
    lifecycle.run(executor);
    const auto retried = std::any_cast<SubflowResult>(
        lifecycle.get_latest_output("retry", "subflow_result"));
    assert(retried.ok);
    assert(retried.attempt == 3);
    assert(retried.mode == SubflowStartMode::Retry);
    const auto restarted = std::any_cast<SubflowResult>(
        lifecycle.get_latest_output("restart", "subflow_result"));
    const auto resumed = std::any_cast<SubflowResult>(
        lifecycle.get_latest_output("resume", "subflow_result"));
    assert(restarted.ok && restarted.mode == SubflowStartMode::Restart);
    assert(resumed.ok && resumed.mode == SubflowStartMode::Resume);

    workflow::GraphBuilder cancelled("cancelled_subflow");
    cancelled.create_any_source("input", {{"value", 1}});
    SubflowLimits cancel_limits;
    cancel_limits.cancel_requested = std::make_shared<std::atomic_bool>(true);
    SubflowNode::create(
        cancelled, "cancelled", {{"input", "value"}},
        [](const SubflowRequest&) {
            SubflowResult result;
            result.ok = true;
            return result;
        }, cancel_limits);
    cancelled.run(executor);
    const auto cancel_result = std::any_cast<SubflowResult>(
        cancelled.get_latest_output("cancelled", "subflow_result"));
    assert(!cancel_result.ok);
    assert(cancel_result.error == "cancelled");

    workflow::GraphBuilder expired("expired_subflow");
    expired.create_any_source("input", {{"value", 1}});
    SubflowLimits deadline_limits;
    deadline_limits.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
    SubflowNode::create(
        expired, "expired", {{"input", "value"}},
        [](const SubflowRequest&) {
            SubflowResult result;
            result.ok = true;
            return result;
        }, deadline_limits);
    expired.run(executor);
    const auto deadline_result = std::any_cast<SubflowResult>(
        expired.get_latest_output("expired", "subflow_result"));
    assert(!deadline_result.ok);
    assert(deadline_result.error == "deadline_exceeded");
}
