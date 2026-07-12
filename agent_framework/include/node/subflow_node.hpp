#ifndef AGENT_FRAMEWORK_NODE_SUBFLOW_NODE_HPP
#define AGENT_FRAMEWORK_NODE_SUBFLOW_NODE_HPP

#include <workflow/nodeflow.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace agent_framework::node {

struct SubflowLimits {
    std::size_t max_depth = 8;
    std::size_t max_iterations = 32;
    std::size_t max_tool_calls = 128;
    std::shared_ptr<std::atomic_bool> cancel_requested;
    std::optional<std::chrono::steady_clock::time_point> deadline;
};

struct SubflowUsage {
    std::size_t iterations = 0;
    std::size_t tool_calls = 0;
};

enum class SubflowStartMode { Start, Retry, Restart, Resume };

struct SubflowRequest {
    workflow::ValueMap inputs;
    std::string parent_run_id;
    std::string run_id;
    std::string subtask_id;
    std::size_t attempt = 0;
    SubflowStartMode mode = SubflowStartMode::Start;
    SubflowLimits limits;
};

struct SubflowResult {
    bool ok = false;
    workflow::ValueMap outputs;
    std::string error;
    SubflowUsage usage;
    std::string run_id;
    std::size_t attempt = 0;
    SubflowStartMode mode = SubflowStartMode::Start;
};

using SubflowRunner = std::function<SubflowResult(const SubflowRequest&)>;

class SubflowNode {
public:
    static std::pair<std::shared_ptr<workflow::AnyNode>, tf::Task> create(
        workflow::GraphBuilder& builder,
        const std::string& name,
        const std::vector<std::pair<std::string, std::string>>& input_specs,
        SubflowRunner runner,
        SubflowLimits limits = {},
        std::size_t attempt = 0,
        SubflowStartMode mode = SubflowStartMode::Start,
        const std::string& output_key = "subflow_result");
};

}  // namespace agent_framework::node

#endif
