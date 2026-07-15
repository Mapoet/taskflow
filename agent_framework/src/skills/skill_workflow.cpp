#include <agent/skill_workflow.hpp>
#include <agent/skill_lifecycle.hpp>

#include <workflow/nodeflow.hpp>

#include <algorithm>
#include <any>
#include <mutex>
#include <set>
#include <stdexcept>
#include <utility>

namespace agent_framework {
namespace {

using json = nlohmann::json;

json failure(const char* code, const std::string& message,
             json details = json::object()) {
    return {{"error", message}, {"code", code}, {"details", std::move(details)}};
}

class WorkflowError : public std::runtime_error {
public:
    explicit WorkflowError(json error)
        : std::runtime_error(error.value("error", "workflow failed")), error_(std::move(error)) {}
    const json& error() const noexcept { return error_; }
private:
    json error_;
};

bool valid_pointer(const std::string& pointer) {
    if (!pointer.empty() && pointer.front() != '/') return false;
    try {
        (void)json::json_pointer(pointer);
        return true;
    } catch (...) {
        return false;
    }
}

json mapped_value(const json& value, const std::string& pointer) {
    try {
        return pointer.empty() ? value : value.at(json::json_pointer(pointer));
    } catch (const std::exception& error) {
        throw WorkflowError(failure(kSkillWorkflowMappingInvalid,
            "workflow JSON mapping failed", {{"path", pointer}, {"reason", error.what()}}));
    }
}

struct Mapping {
    std::string from;
    std::string path;
};

Mapping parse_mapping(const json& value) {
    if (!value.is_object() || !value.contains("from") || !value.at("from").is_string())
        throw WorkflowError(failure(kSkillWorkflowMappingInvalid,
            "mapping requires a string from field"));
    Mapping out{value.at("from").get<std::string>(), value.value("path", "")};
    if (!valid_pointer(out.path))
        throw WorkflowError(failure(kSkillWorkflowMappingInvalid,
            "mapping path is not a valid JSON Pointer", {{"path", out.path}}));
    return out;
}

std::map<std::string, Mapping> parse_mappings(const json& value) {
    if (!value.is_object())
        throw WorkflowError(failure(kSkillWorkflowMappingInvalid,
                                    "node input must be an object"));
    std::map<std::string, Mapping> out;
    for (auto it = value.begin(); it != value.end(); ++it)
        out.emplace(it.key(), parse_mapping(it.value()));
    return out;
}

std::vector<std::pair<std::string, std::string>> input_ports(
    const std::map<std::string, Mapping>& mappings) {
    std::vector<std::pair<std::string, std::string>> out;
    std::set<std::string> seen;
    for (const auto& [_, mapping] : mappings) {
        const std::string node = mapping.from == "$input" ? "$input" : mapping.from;
        const std::string key = mapping.from == "$input" ? "$input" : mapping.from;
        if (seen.insert(key).second) out.emplace_back(node, key);
    }
    return out;
}

json build_arguments(const workflow::ValueMap& values,
                     const std::map<std::string, Mapping>& mappings) {
    json out = json::object();
    for (const auto& [name, mapping] : mappings) {
        const std::string key = mapping.from == "$input" ? "$input" : mapping.from;
        const auto found = values.find(key);
        if (found == values.end())
            throw WorkflowError(failure(kSkillWorkflowMappingInvalid,
                "mapped source is unavailable", {{"source", mapping.from}}));
        const auto* source = std::any_cast<json>(&found->second);
        if (!source)
            throw WorkflowError(failure(kSkillWorkflowMappingInvalid,
                "mapped source is not JSON", {{"source", mapping.from}}));
        out[name] = mapped_value(*source, mapping.path);
    }
    return out;
}

bool json_condition(const json& value, const json& condition) {
    const json actual = mapped_value(value, condition.value("path", ""));
    const std::string op = condition.value("op", "eq");
    const json expected = condition.value("value", json());
    if (op == "eq") return actual == expected;
    if (op == "ne") return actual != expected;
    if (!actual.is_number() || !expected.is_number())
        throw WorkflowError(failure(kSkillWorkflowDescriptorInvalid,
            "ordered loop comparisons require numbers", {{"op", op}}));
    const double lhs = actual.get<double>();
    const double rhs = expected.get<double>();
    if (op == "lt") return lhs < rhs;
    if (op == "lte") return lhs <= rhs;
    if (op == "gt") return lhs > rhs;
    if (op == "gte") return lhs >= rhs;
    throw WorkflowError(failure(kSkillWorkflowDescriptorInvalid,
        "unsupported loop condition operator", {{"op", op}}));
}

SkillPermissionGrant parse_grants(const json& value,
                                  const SkillPermissionGrant& inherited) {
    if (value.is_null()) return inherited;
    if (!value.is_object())
        throw WorkflowError(failure(kSkillWorkflowDescriptorInvalid,
                                    "child permissions must be an object"));
    SkillPermissionGrant out;
    auto read = [&](const char* name, std::vector<std::string>& target) {
        if (!value.contains(name)) return;
        if (!value.at(name).is_array())
            throw WorkflowError(failure(kSkillWorkflowDescriptorInvalid,
                "child permission list must be an array", {{"permission", name}}));
        for (const auto& item : value.at(name)) {
            if (!item.is_string())
                throw WorkflowError(failure(kSkillWorkflowDescriptorInvalid,
                    "child permission entries must be strings", {{"permission", name}}));
            target.push_back(item.get<std::string>());
        }
    };
    read("tools", out.tools);
    read("network", out.network);
    read("environment", out.environment);
    read("filesystem-read", out.filesystem_read);
    read("filesystem-write", out.filesystem_write);
    read("secrets", out.secrets);
    std::string reason;
    if (!child_task_grants_are_narrower(inherited, out, &reason))
        throw WorkflowError(failure(kSkillPermissionDenied,
                                    "child permissions cannot escalate", {{"reason", reason}}));
    return out;
}

struct PinnedSkill {
    SkillIndexEntry entry;
    std::shared_ptr<const SkillManifest> manifest;
    std::shared_ptr<SkillCapabilityBinding> binding;
    std::map<std::string, json> workflows;
};

struct ExecutionState {
    json checkpoint = json::object();
    json events = json::array();
    std::map<std::string, json> loop_errors;
    std::mutex mutex;
};

struct Execution {
    std::map<std::string, PinnedSkill> skills;
    SkillWorkflowRunOptions options;
    std::shared_ptr<ExecutionState> state;
    std::string root_skill;

    bool stopped() const {
        if (!options.context.control) return false;
        options.context.control->check_deadline_now();
        return options.context.control->is_cancel_requested() ||
               options.context.control->is_deadline_exceeded();
    }

    void event(const std::string& type, const std::string& path,
               json details = json::object()) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->events.push_back({{"type", type}, {"path", path},
                                 {"attempt", options.context.attempt},
                                 {"details", std::move(details)}});
    }

    std::optional<json> completed(const std::string& path) {
        std::lock_guard<std::mutex> lock(state->mutex);
        const json& values = state->checkpoint["completed"];
        if (!values.contains(path)) return std::nullopt;
        return values.at(path);
    }

    void commit(const std::string& path, const json& value) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->checkpoint["completed"][path] = value;
    }

    json invoke_action(const json& action, const json& input,
                       const std::string& path, std::size_t depth) {
        if (stopped()) throw WorkflowError(failure(kSkillCancelled, "workflow cancelled"));
        const std::string type = action.value("type", "tool");
        const std::string skill = action.value("skill", root_skill);
        const std::string resource = action.value("resource", "");
        if (type == "tool") {
            auto skill_it = skills.find(skill);
            if (skill_it == skills.end())
                throw WorkflowError(failure(kSkillWorkflowDependencyMismatch,
                    "workflow dependency is not pinned", {{"skill", skill}}));
            const auto side_effect = skill_it->second.binding->capability_side_effect(resource);
            if (!side_effect)
                throw WorkflowError(failure(kSkillDependencyUnavailable,
                    "workflow capability is unavailable", {{"skill", skill}, {"resource", resource}}));
            const std::string idempotency = action.value("idempotency-key", "");
            if (*side_effect != ToolSideEffect::ReadOnly &&
                options.mode != SkillWorkflowStartMode::Start && idempotency.empty())
                throw WorkflowError(failure(kSkillWorkflowReplayDenied,
                    "side-effect capability replay requires an idempotency key",
                    {{"path", path}, {"resource", resource}}));
            if (!idempotency.empty()) {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->checkpoint["idempotency"].contains(idempotency))
                    return state->checkpoint["idempotency"].at(idempotency);
            }
            ToolCallControl control;
            control.cancellation_requested = [this] { return stopped(); };
            json output = skill_it->second.binding->invoke_capability(resource, input, control);
            if (output.is_object() && output.contains("error") && output.contains("code"))
                throw WorkflowError(output);
            if (!idempotency.empty()) {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->checkpoint["idempotency"][idempotency] = output;
            }
            return output;
        }
        if (type == "workflow") {
            if (depth >= options.max_depth)
                throw WorkflowError(failure(kSkillResourceBudgetExceeded,
                                             "workflow nesting depth exceeded"));
            return execute_descriptor(skill, resource, input, path, depth + 1);
        }
        throw WorkflowError(failure(kSkillWorkflowDescriptorInvalid,
            "unsupported action type", {{"type", type}}));
    }

    json load_descriptor(const std::string& skill, const std::string& resource_id) {
        const auto pinned = skills.find(skill);
        if (pinned == skills.end())
            throw WorkflowError(failure(kSkillWorkflowDependencyMismatch,
                "workflow dependency is not pinned", {{"skill", skill}}));
        const auto descriptor = pinned->second.workflows.find(resource_id);
        if (descriptor == pinned->second.workflows.end())
            throw WorkflowError(failure(kSkillDependencyUnavailable,
                "workflow resource is unavailable", {{"skill", skill}, {"resource", resource_id}}));
        return descriptor->second;
    }

    json execute_descriptor(const std::string& skill, const std::string& resource_id,
                            const json& input, const std::string& prefix,
                            std::size_t depth) {
        const json descriptor = load_descriptor(skill, resource_id);
        workflow::GraphBuilder graph("skill_workflow_" + resource_id);
        auto cancel = options.context.control
            ? options.context.control->cancellation_token()
            : std::make_shared<std::atomic_bool>(false);
        const auto deadline = options.context.control
            ? options.context.control->working_deadline() : std::nullopt;
        graph.configure_run_context(cancel, deadline, options.max_depth);
        graph.create_any_source("$input", {{"$input", input}});
        std::map<std::string, std::shared_ptr<workflow::LoopNode>> loops;

        for (const auto& node : descriptor.at("nodes")) {
            const std::string id = node.at("id").get<std::string>();
            const std::string type = node.at("type").get<std::string>();
            const std::string path = prefix + "/" + id;
            const auto mappings = parse_mappings(node.value("input", json::object()));
            auto ports = input_ports(mappings);
            if (auto value = completed(path)) {
                graph.create_any_node(
                    id, ports, [id, value](const workflow::ValueMap&) {
                        return workflow::ValueMap{{id, *value}};
                    }, {id});
                continue;
            }
            if (type == "loop") {
                const json action = node.at("body");
                const json condition = node.at("condition");
                const std::size_t max_iterations = node.value("max-iterations", 32U);
                std::size_t iteration_offset = 0;
                json resumed_state;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    iteration_offset = state->checkpoint["iterations"].value(path, 0U);
                    if (state->checkpoint["loopState"].contains(path))
                        resumed_state = state->checkpoint["loopState"].at(path);
                }
                if (!resumed_state.is_null()) {
                    const std::string resume_key = id + "__resume";
                    graph.create_any_source(resume_key, {{resume_key, resumed_state}});
                    ports = {{resume_key, resume_key}};
                }
                workflow::LoopOptions loop_options;
                loop_options.max_iterations = max_iterations > iteration_offset
                    ? max_iterations - iteration_offset : 0;
                loop_options.cancel_requested = cancel;
                loop_options.feedback = {{id, ports.at(0).second}};
                auto pair = graph.create_loop(
                    id, ports,
                    [this, action, path, id, cancel, iteration_offset](
                        const workflow::ValueMap& values,
                        const workflow::IterationContext& context) {
                        if (stopped()) {
                            cancel->store(true, std::memory_order_release);
                            throw WorkflowError(failure(kSkillCancelled, "workflow cancelled"));
                        }
                        const auto source = std::any_cast<json>(&values.begin()->second);
                        if (!source) throw WorkflowError(failure(
                            kSkillWorkflowMappingInvalid, "loop state is not JSON"));
                        json iteration_action = action;
                        const std::string key = action.value("idempotency-key", "");
                        if (!key.empty())
                            iteration_action["idempotency-key"] = key + "/" +
                                std::to_string(iteration_offset + context.iteration);
                        json output;
                        try {
                            output = invoke_action(iteration_action, *source, path + "/body",
                                                   context.depth);
                        } catch (const WorkflowError& error) {
                            std::lock_guard<std::mutex> lock(state->mutex);
                            state->loop_errors[path] = error.error();
                            throw;
                        }
                        if (stopped()) {
                            cancel->store(true, std::memory_order_release);
                            throw WorkflowError(failure(kSkillCancelled, "workflow cancelled"));
                        }
                        {
                            std::lock_guard<std::mutex> lock(state->mutex);
                            state->checkpoint["iterations"][path] =
                                iteration_offset + context.iteration + 1;
                            state->checkpoint["loopState"][path] = output;
                        }
                        event("iteration_completed", path,
                              {{"iteration", iteration_offset + context.iteration}});
                        return workflow::ValueMap{{id, output}};
                    },
                    [condition, id](const workflow::ValueMap& values,
                                    const workflow::IterationContext&) {
                        const auto* output = std::any_cast<json>(&values.at(id));
                        if (!output) throw WorkflowError(failure(
                            kSkillWorkflowMappingInvalid, "loop output is not JSON"));
                        return json_condition(*output, condition)
                            ? workflow::LoopDecision::Exit
                            : workflow::LoopDecision::Continue;
                    },
                    [id](const workflow::ValueMap& values,
                         const workflow::IterationContext&) {
                        const auto found = values.find(id);
                        return workflow::ValueMap{{id, found != values.end()
                            ? found->second : values.begin()->second}};
                    }, {id}, loop_options);
                loops.emplace(id, pair.first);
                continue;
            }

            if (type == "workflow") {
                graph.create_subtask_module(
                    id, ports,
                    [this, node, mappings, path, id, depth](
                        workflow::GraphBuilder& nested, const workflow::ValueMap& values,
                        const workflow::RunContext& context) {
                        const json arguments = build_arguments(values, mappings);
                        nested.create_any_source("nested_input", {{"arguments", arguments}});
                        nested.create_any_node(
                            "nested_result", {{"nested_input", "arguments"}},
                            [this, node, path, id, depth, context](const workflow::ValueMap& input_values) {
                                if (auto value = completed(path))
                                    return workflow::ValueMap{{id, *value}};
                                const auto* nested_input =
                                    std::any_cast<json>(&input_values.at("arguments"));
                                if (!nested_input) throw WorkflowError(failure(
                                    kSkillWorkflowMappingInvalid, "nested workflow input is not JSON"));
                                event("node_started", path);
                                json output = invoke_action(node, *nested_input, path,
                                                            std::max(depth, context.depth));
                                commit(path, output);
                                event("node_completed", path);
                                return workflow::ValueMap{{id, output}};
                            }, {id});
                        return workflow::OutputBindings{{id, {"nested_result", id}}};
                    }, {id}, {.max_depth = options.max_depth});
                continue;
            }

            graph.create_any_node(
                id, ports,
                [this, node, mappings, path, id, depth](const workflow::ValueMap& values) {
                    if (auto value = completed(path))
                        return workflow::ValueMap{{id, *value}};
                    const json arguments = build_arguments(values, mappings);
                    event("node_started", path);
                    json output;
                    const std::string type = node.at("type").get<std::string>();
                    if (type == "tool" || type == "workflow") {
                        output = invoke_action(node, arguments, path, depth);
                    } else if (type == "child") {
                        const std::string idempotency = node.value("idempotency-key", "");
                        if (options.mode != SkillWorkflowStartMode::Start && idempotency.empty())
                            throw WorkflowError(failure(kSkillWorkflowReplayDenied,
                                "child task replay requires an idempotency key", {{"path", path}}));
                        std::optional<json> replayed;
                        if (!idempotency.empty()) {
                            std::lock_guard<std::mutex> lock(state->mutex);
                            if (state->checkpoint["idempotency"].contains(idempotency))
                                replayed = state->checkpoint["idempotency"].at(idempotency);
                        }
                        if (replayed) {
                            commit(path, *replayed);
                            return workflow::ValueMap{{id, *replayed}};
                        }
                        if (!options.child_backend)
                            throw WorkflowError(failure(kSkillDependencyUnavailable,
                                                        "child backend resolver is unavailable"));
                        const std::string backend_name = node.value("backend", "local");
                        auto backend = options.child_backend(backend_name);
                        if (!backend)
                            throw WorkflowError(failure(kSkillDependencyUnavailable,
                                "child backend is unavailable", {{"backend", backend_name}}));
                        ChildTaskRequest request;
                        request.child_id = id;
                        request.parent_run_id = options.context.run_id;
                        request.run_id = options.context.run_id + "/" + id;
                        request.trace_id = options.context.task_id;
                        request.idempotency_key = node.value("idempotency-key", "");
                        request.depth = depth + 1;
                        request.attempt = static_cast<std::size_t>(options.context.attempt);
                        request.mode = options.mode == SkillWorkflowStartMode::Retry
                            ? ChildTaskStartMode::Retry
                            : options.mode == SkillWorkflowStartMode::Restart
                                ? ChildTaskStartMode::Restart
                                : options.mode == SkillWorkflowStartMode::Resume
                                    ? ChildTaskStartMode::Resume : ChildTaskStartMode::Start;
                        request.inputs = arguments;
                        request.grants = parse_grants(node.value("permissions", json()),
                                                      options.context.grants);
                        request.policy.max_depth = options.max_depth;
                        if (options.context.control) {
                            request.policy.cancel_requested =
                                options.context.control->cancellation_token();
                            request.policy.deadline =
                                options.context.control->working_deadline();
                        }
                        request.policy.max_input_bytes = options.context.limits.max_input_bytes;
                        request.policy.max_output_bytes = options.context.limits.max_output_bytes;
                        {
                            std::lock_guard<std::mutex> lock(state->mutex);
                            request.checkpoint = state->checkpoint.value("children", json::object())
                                .value(path, json::object());
                        }
                        ChildTaskResult child = backend->start(std::move(request))->wait();
                        if (!child.ok())
                            throw WorkflowError(failure(
                                child.error_code.empty() ? kSkillWorkflowChildFailed
                                                         : child.error_code.c_str(),
                                child.error.value_or("child task failed"),
                                {{"status", child_task_status_cstr(child.status)}}));
                        {
                            std::lock_guard<std::mutex> lock(state->mutex);
                            state->checkpoint["children"][path] = child.checkpoint;
                        }
                        output = child.outputs;
                        if (!idempotency.empty()) {
                            std::lock_guard<std::mutex> lock(state->mutex);
                            state->checkpoint["idempotency"][idempotency] = output;
                        }
                    }
                    if (output.dump().size() > options.context.limits.max_output_bytes)
                        throw WorkflowError(failure(kSkillResourceBudgetExceeded,
                                                    "workflow node output budget exceeded"));
                    commit(path, output);
                    event("node_completed", path);
                    return workflow::ValueMap{{id, output}};
                }, {id});
        }

        tf::Executor executor;
        graph.run(executor);
        for (const auto& [id, loop] : loops) {
            const auto result = loop->last_result();
            const std::string path = prefix + "/" + id;
            if (result.status == workflow::LoopStatus::Cancelled || stopped())
                throw WorkflowError(failure(kSkillCancelled, "workflow loop cancelled"));
            if (result.status == workflow::LoopStatus::DeadlineExceeded)
                throw WorkflowError(failure(kSkillCancelled, "workflow deadline exceeded"));
            if (result.status == workflow::LoopStatus::BodyError ||
                result.status == workflow::LoopStatus::ConditionError ||
                result.status == workflow::LoopStatus::ExitError) {
                std::lock_guard<std::mutex> lock(state->mutex);
                const auto error = state->loop_errors.find(path);
                if (error != state->loop_errors.end()) throw WorkflowError(error->second);
                throw WorkflowError(failure(kSkillWorkflowDescriptorInvalid,
                    "workflow loop failed", {{"reason", result.error}}));
            }
            const auto* value = std::any_cast<json>(&result.outputs.at(id));
            if (!value) throw WorkflowError(failure(
                kSkillWorkflowMappingInvalid, "workflow loop result is not JSON"));
            commit(path, *value);
            event("loop_completed", path, {{"iterations", result.iterations}});
        }

        json output = json::object();
        for (auto it = descriptor.at("outputs").begin();
             it != descriptor.at("outputs").end(); ++it) {
            const Mapping mapping = parse_mapping(it.value());
            const std::string node = mapping.from == "$input" ? "$input" : mapping.from;
            const std::string key = mapping.from == "$input" ? "$input" : mapping.from;
            const auto value = graph.get_latest_output(node, key);
            const auto* source = std::any_cast<json>(&value);
            if (!source) throw WorkflowError(failure(
                kSkillWorkflowMappingInvalid, "workflow output source is not JSON"));
            output[it.key()] = mapped_value(*source, mapping.path);
        }
        return output;
    }
};

} // namespace

SkillWorkflowValidationResult validate_skill_workflow_descriptor(const json& descriptor) {
    try {
        if (!descriptor.is_object() ||
            descriptor.value("api-version", "") != "agent.taskflow/workflow/v1" ||
            descriptor.value("kind", "") != "SkillWorkflow")
            throw WorkflowError(failure(kSkillWorkflowDescriptorInvalid,
                "workflow requires api-version agent.taskflow/workflow/v1 and kind SkillWorkflow"));
        if (!descriptor.contains("nodes") || !descriptor.at("nodes").is_array() ||
            !descriptor.contains("outputs") || !descriptor.at("outputs").is_object())
            throw WorkflowError(failure(kSkillWorkflowDescriptorInvalid,
                "workflow requires nodes array and outputs object"));
        std::set<std::string> available{"$input"};
        for (const auto& node : descriptor.at("nodes")) {
            if (!node.is_object() || !node.contains("id") || !node.at("id").is_string() ||
                !node.contains("type") || !node.at("type").is_string())
                throw WorkflowError(failure(kSkillWorkflowDescriptorInvalid,
                                             "each workflow node requires string id and type"));
            const std::string id = node.at("id").get<std::string>();
            const std::string type = node.at("type").get<std::string>();
            if (id.empty() || id == "$input" || !available.insert(id).second)
                throw WorkflowError(failure(kSkillWorkflowDescriptorInvalid,
                    "workflow node id is empty, reserved, or duplicated", {{"id", id}}));
            if (type != "tool" && type != "workflow" && type != "loop" && type != "child")
                throw WorkflowError(failure(kSkillWorkflowDescriptorInvalid,
                    "unsupported workflow node type", {{"type", type}}));
            const auto mappings = parse_mappings(node.value("input", json::object()));
            for (const auto& [_, mapping] : mappings)
                if (!available.contains(mapping.from) || mapping.from == id)
                    throw WorkflowError(failure(kSkillWorkflowMappingInvalid,
                        "workflow input must reference $input or an earlier node",
                        {{"node", id}, {"source", mapping.from}}));
            if (type == "loop") {
                if (mappings.size() != 1 || !node.contains("body") ||
                    !node.at("body").is_object() || !node.contains("condition") ||
                    !node.at("condition").is_object())
                    throw WorkflowError(failure(kSkillWorkflowDescriptorInvalid,
                        "loop requires one input mapping, body, and condition"));
                const std::string body_type = node.at("body").value("type", "tool");
                if ((body_type != "tool" && body_type != "workflow") ||
                    node.at("body").value("resource", "").empty())
                    throw WorkflowError(failure(kSkillWorkflowDescriptorInvalid,
                        "loop body must reference a tool or workflow resource"));
                if (!valid_pointer(node.at("condition").value("path", "")))
                    throw WorkflowError(failure(kSkillWorkflowDescriptorInvalid,
                                                 "loop condition path is invalid"));
            } else if ((type == "tool" || type == "workflow") &&
                       node.value("resource", "").empty()) {
                throw WorkflowError(failure(kSkillWorkflowDescriptorInvalid,
                                             "workflow action resource is required"));
            }
        }
        for (auto it = descriptor.at("outputs").begin();
             it != descriptor.at("outputs").end(); ++it) {
            const Mapping mapping = parse_mapping(it.value());
            if (!available.contains(mapping.from))
                throw WorkflowError(failure(kSkillWorkflowMappingInvalid,
                    "workflow output references an unavailable node", {{"source", mapping.from}}));
        }
        return {true, json::object()};
    } catch (const WorkflowError& error) {
        return {false, error.error()};
    } catch (const std::exception& error) {
        return {false, failure(kSkillWorkflowDescriptorInvalid, error.what())};
    }
}

SkillWorkflowRuntime::SkillWorkflowRuntime(
    std::shared_ptr<SkillRegistry> registry, std::shared_ptr<SkillLoader> loader,
    std::shared_ptr<SkillRuntime> skill_runtime,
    std::shared_ptr<SkillCapabilityRuntime> capabilities)
    : registry_(std::move(registry)), loader_(std::move(loader)),
      skill_runtime_(std::move(skill_runtime)), capabilities_(std::move(capabilities)) {
    if (!registry_ || !loader_ || !skill_runtime_ || !capabilities_)
        throw std::invalid_argument("SkillWorkflowRuntime requires all services");
}

SkillWorkflowResult SkillWorkflowRuntime::run(
    const std::string& skill_id, const std::string& workflow_resource_id,
    const json& input, SkillWorkflowRunOptions options) const {
    SkillWorkflowResult result;
    auto run_state = std::make_shared<ExecutionState>();
    try {
        const auto registry_snapshot = registry_->snapshot();
        const auto root_entry = registry_snapshot.get(skill_id);
        const auto root_manifest = registry_snapshot.get_manifest(skill_id);
        if (!root_entry || !root_manifest)
            throw WorkflowError(failure(kSkillDependencyUnavailable,
                                        "workflow skill is unavailable"));
        Execution execution{{}, std::move(options), run_state, skill_id};
        std::function<void(const std::string&, const std::string&)> pin;
        pin = [&](const std::string& id, const std::string& expected_range) {
            const auto existing = execution.skills.find(id);
            if (existing != execution.skills.end()) {
                auto version = SkillSemVersion::parse(existing->second.manifest->version);
                auto range = SkillSemVersionRange::parse(expected_range.empty() ? "*" : expected_range);
                if (!version || !range || !range->contains(*version))
                    throw WorkflowError(failure(kSkillWorkflowDependencyMismatch,
                        "workflow dependency does not satisfy the pinned range",
                        {{"skill", id}, {"expected", expected_range},
                         {"actual", existing->second.manifest->version}}));
                return;
            }
            const auto entry = registry_snapshot.get(id);
            const auto manifest = registry_snapshot.get_manifest(id);
            if (!entry || !manifest)
                throw WorkflowError(failure(kSkillDependencyUnavailable,
                    "workflow dependency is unavailable", {{"skill", id}}));
            auto version = SkillSemVersion::parse(manifest->version);
            auto range = SkillSemVersionRange::parse(expected_range.empty() ? "*" : expected_range);
            if (!version || !range || !range->contains(*version))
                throw WorkflowError(failure(kSkillWorkflowDependencyMismatch,
                    "workflow dependency version does not satisfy the snapshot lock",
                    {{"skill", id}, {"expected", expected_range},
                     {"actual", manifest->version}}));
            auto bound = capabilities_->bind_snapshot(
                *entry, manifest, execution.options.context,
                {.publish_to_toolbus = false});
            if (!bound.ok()) throw WorkflowError(bound.error);
            PinnedSkill pinned{*entry, manifest, bound.binding, {}};
            for (const auto& resource : manifest->resources) {
                if (resource.kind != SkillResourceType::Workflow) continue;
                std::string load_error;
                auto content = loader_->load_resource_snapshot(
                    *entry, manifest, resource.path, SkillResourceKind::Workflow,
                    resource.size_limit.value_or(1024U * 1024U), &load_error);
                if (!content)
                    throw WorkflowError(failure(kSkillDependencyUnavailable,
                        "workflow snapshot load failed",
                        {{"skill", id}, {"resource", resource.id}, {"reason", load_error}}));
                try {
                    json descriptor = json::parse(*content);
                    auto valid = validate_skill_workflow_descriptor(descriptor);
                    if (!valid.ok) throw WorkflowError(valid.error);
                    pinned.workflows.emplace(resource.id, std::move(descriptor));
                } catch (const WorkflowError&) {
                    throw;
                } catch (const std::exception& parse_error) {
                    throw WorkflowError(failure(kSkillWorkflowDescriptorInvalid,
                        "workflow snapshot JSON is invalid",
                        {{"skill", id}, {"resource", resource.id},
                         {"reason", parse_error.what()}}));
                }
            }
            execution.skills.emplace(id, std::move(pinned));
            result.dependency_lock[id] = manifest->version;
            for (const auto& dependency : manifest->dependencies) {
                if (dependency.optional && !registry_snapshot.get(dependency.name)) continue;
                pin(dependency.name, dependency.version);
            }
        };
        pin(skill_id, root_manifest->version);

        const std::string identity = skill_id + "@" + root_manifest->version + "/" +
                                     workflow_resource_id;
        json checkpoint = execution.options.checkpoint;
        if (execution.options.mode == SkillWorkflowStartMode::Start || checkpoint.empty())
            checkpoint = json::object();
        if (execution.options.mode == SkillWorkflowStartMode::Retry ||
            execution.options.mode == SkillWorkflowStartMode::Resume ||
            execution.options.mode == SkillWorkflowStartMode::Restart) {
            if (checkpoint.value("identity", "") != identity)
                throw WorkflowError(failure(kSkillWorkflowCheckpointIncompatible,
                                             "workflow checkpoint identity does not match"));
        }
        if (execution.options.mode == SkillWorkflowStartMode::Restart) {
            const json ledger = checkpoint.value("idempotency", json::object());
            checkpoint = {{"idempotency", ledger}};
            ++execution.options.context.attempt;
        } else if (execution.options.mode == SkillWorkflowStartMode::Retry) {
            ++execution.options.context.attempt;
        }
        checkpoint["identity"] = identity;
        checkpoint["attempt"] = execution.options.context.attempt;
        if (!checkpoint.contains("completed")) checkpoint["completed"] = json::object();
        if (!checkpoint.contains("idempotency")) checkpoint["idempotency"] = json::object();
        if (!checkpoint.contains("iterations")) checkpoint["iterations"] = json::object();
        if (!checkpoint.contains("loopState")) checkpoint["loopState"] = json::object();
        if (!checkpoint.contains("children")) checkpoint["children"] = json::object();
        execution.state->checkpoint = std::move(checkpoint);
        execution.event("workflow_started", skill_id + "/" + workflow_resource_id,
                        {{"mode", skill_workflow_start_mode_cstr(execution.options.mode)}});

        auto begun = skill_runtime_->begin_snapshot(
            *root_entry, root_manifest, workflow_resource_id, SkillResourceType::Workflow,
            input, execution.options.context);
        if (!begun.ok || !begun.ticket) throw WorkflowError(begun.error);
        json output = execution.execute_descriptor(
            skill_id, workflow_resource_id, input,
            skill_id + "/" + workflow_resource_id, 0);
        auto finished = skill_runtime_->finish(*begun.ticket, output);
        if (!finished.ok) throw WorkflowError(finished.error);
        execution.event("workflow_completed", skill_id + "/" + workflow_resource_id);
        result.ok = true;
        result.output = std::move(output);
        {
            std::lock_guard<std::mutex> lock(execution.state->mutex);
            result.checkpoint = execution.state->checkpoint;
            result.events = execution.state->events;
        }
    } catch (const WorkflowError& error) {
        result.error = error.error();
    } catch (const std::exception& error) {
        result.error = failure(kSkillWorkflowDescriptorInvalid, error.what());
    }
    if (!result.ok) {
        std::lock_guard<std::mutex> lock(run_state->mutex);
        result.checkpoint = run_state->checkpoint;
        result.events = run_state->events;
    }
    return result;
}

const char* skill_workflow_start_mode_cstr(SkillWorkflowStartMode mode) noexcept {
    switch (mode) {
    case SkillWorkflowStartMode::Start: return "start";
    case SkillWorkflowStartMode::Retry: return "retry";
    case SkillWorkflowStartMode::Restart: return "restart";
    case SkillWorkflowStartMode::Resume: return "resume";
    }
    return "start";
}

} // namespace agent_framework
