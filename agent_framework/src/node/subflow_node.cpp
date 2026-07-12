#include <node/subflow_node.hpp>

#include <stdexcept>

namespace agent_framework::node {

std::pair<std::shared_ptr<workflow::AnyNode>, tf::Task> SubflowNode::create(
    workflow::GraphBuilder& builder,
    const std::string& name,
    const std::vector<std::pair<std::string, std::string>>& input_specs,
    SubflowRunner runner,
    SubflowLimits limits,
    std::size_t attempt,
    SubflowStartMode mode,
    const std::string& output_key) {
    if (!runner) {
        throw std::invalid_argument("SubflowNode requires a runner");
    }
    if (output_key.empty()) {
        throw std::invalid_argument("SubflowNode output key must not be empty");
    }

    workflow::SubflowOptions options;
    options.max_depth = limits.max_depth;
    options.attempt = attempt;
    return builder.create_subtask_module(
        name,
        input_specs,
        [runner = std::move(runner), limits = std::move(limits), attempt, mode, output_key](
            workflow::GraphBuilder& nested,
            const workflow::ValueMap& inputs,
            const workflow::RunContext& context) {
            SubflowRequest request;
            request.inputs = inputs;
            request.parent_run_id = context.parent_run_id;
            request.run_id = context.run_id;
            request.subtask_id = context.subtask_id;
            request.attempt = attempt;
            request.mode = mode;
            request.limits = limits;

            SubflowResult result;
            result.run_id = context.run_id;
            result.attempt = attempt;
            result.mode = mode;
            if (limits.cancel_requested && limits.cancel_requested->load()) {
                result.error = "cancelled";
            } else if (limits.deadline && std::chrono::steady_clock::now() >= *limits.deadline) {
                result.error = "deadline_exceeded";
            } else if (limits.max_iterations == 0 || limits.max_tool_calls == 0) {
                result.error = "budget_exceeded";
            } else {
                try {
                    result = runner(request);
                    result.run_id = context.run_id;
                    result.attempt = attempt;
                    result.mode = mode;
                    if (result.usage.iterations > limits.max_iterations ||
                        result.usage.tool_calls > limits.max_tool_calls) {
                        result.ok = false;
                        result.error = "budget_exceeded";
                        result.outputs.clear();
                    }
                } catch (const std::exception& e) {
                    result.ok = false;
                    result.error = e.what();
                } catch (...) {
                    result.ok = false;
                    result.error = "unknown subflow exception";
                }
            }

            nested.create_any_source("result", {{output_key, std::any{result}}});
            return workflow::OutputBindings{{output_key, {"result", output_key}}};
        },
        {output_key},
        options);
}

}  // namespace agent_framework::node
