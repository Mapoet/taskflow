/**
 * @file graph_executor.cpp
 * @brief GraphExecutor implementation (WP2.0 run_react_cli + session merge)
 */

#include <agent/graph_executor.hpp>

#include <agent/internal/agent_thread_state.hpp>

#include <cctype>
#include <cstdlib>
#include <ctime>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>

namespace agent_framework {
namespace {

bool agent_log_level_debug() {
    const char* e = std::getenv("AGENT_LOG_LEVEL");
    if (!e || !*e) {
        return false;
    }
    std::string s(e);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s == "debug";
}

bool message_equal_for_merge(const Message& a, const Message& b) {
    if (a.role != b.role || a.content != b.content) {
        return false;
    }
    if (a.tool_call_id != b.tool_call_id) {
        return false;
    }
    if (a.tool_name != b.tool_name) {
        return false;
    }
    if (a.tool_result.has_value() != b.tool_result.has_value()) {
        return false;
    }
    if (a.tool_result && b.tool_result && *a.tool_result != *b.tool_result) {
        return false;
    }
    return true;
}

void validate_react_cli_request(const ReactCliRunRequest& r) {
    if (!r.session) {
        throw std::invalid_argument("run_react_cli_sync: session is null");
    }
    if (!r.deps.llm) {
        throw std::invalid_argument("run_react_cli_sync: deps.llm is null");
    }
    if (!r.deps.toolbus) {
        throw std::invalid_argument("run_react_cli_sync: deps.toolbus is null");
    }
    if (r.session->initial_user_prompt.empty()) {
        throw std::invalid_argument("run_react_cli_sync: session->initial_user_prompt is empty");
    }
    if (r.options.require_final_json_callback && !r.options.sink.on_final_json) {
        throw std::invalid_argument(
            "run_react_cli_sync: require_final_json_callback but sink.on_final_json is empty");
    }
}

} // namespace

bool merge_react_session_state(
    internal::AgentThreadState& session,
    const std::string& user_turn_snapshot,
    const std::shared_ptr<internal::AgentThreadState>& next) {
    if (user_turn_snapshot.empty()) {
        throw std::invalid_argument("merge_react_session_state: empty user_turn_snapshot");
    }
    if (!next) {
        return false;
    }

    const std::vector<Message>& old_hist = session.history;
    const std::vector<Message>& nh = next->history;
    if (nh.size() < old_hist.size()) {
        return false;
    }
    for (std::size_t i = 0; i < old_hist.size(); ++i) {
        if (!message_equal_for_merge(old_hist[i], nh[i])) {
            return false;
        }
    }

    std::vector<Message> delta(nh.begin() + static_cast<std::ptrdiff_t>(old_hist.size()), nh.end());
    std::vector<Message> new_hist = old_hist;
    Message user_msg;
    user_msg.role = "user";
    user_msg.content = user_turn_snapshot;
    user_msg.timestamp = std::time(nullptr);
    new_hist.push_back(std::move(user_msg));
    new_hist.insert(new_hist.end(), delta.begin(), delta.end());

    session.history = std::move(new_hist);
    session.iteration = next->iteration;
    session.skill_prompt_cache = next->skill_prompt_cache;
    session.active_skill_id = next->active_skill_id;
    session.last_error.clear();
    session.initial_user_prompt.clear();
    return true;
}

void GraphExecutor::build_agent_workflow(const AgentConfig& config,
                                         workflow::GraphBuilder& builder,
                                         const AgentWorkflowDeps& deps,
                                         std::shared_ptr<internal::AgentThreadState> agent_state) {
    build_cli_agent_graph(builder, config, deps, std::move(agent_state));
}

void GraphExecutor::build_agent_workflow(const AgentConfig& config,
                                         workflow::GraphBuilder& builder,
                                         const AgentWorkflowDeps& deps,
                                         std::shared_ptr<internal::AgentThreadState> agent_state,
                                         const CliAgentTerminalSinkOptions& sink) {
    build_cli_agent_graph_with_terminal_sink(builder, config, deps, std::move(agent_state), sink);
}

void GraphExecutor::build_custom_workflow(const WorkflowConfig& /*config*/,
                                            workflow::GraphBuilder& /*builder*/) {
    throw std::logic_error("GraphExecutor::build_custom_workflow: not implemented");
}

void GraphExecutor::register_template(const std::string& name,
                                      std::shared_ptr<WorkflowTemplate> template_ptr) {
    if (!template_ptr) {
        throw std::invalid_argument("GraphExecutor::register_template: null template");
    }
    std::lock_guard<std::mutex> lock(templates_mutex_);
    templates_[name] = std::move(template_ptr);
}

void GraphExecutor::register_react_cli_template() {
    register_template(std::string(kWorkflowTemplateReactCli), std::make_shared<ReActTemplate>());
}

WorkflowResult GraphExecutor::run_react_cli_sync(tf::Executor& executor,
                                                 const ReactCliRunRequest& request) {
    WorkflowResult wr;
    wr.success = false;
    try {
        validate_react_cli_request(request);
    } catch (const std::exception& e) {
        wr.error_message = e.what();
        return wr;
    }

    const std::string u_snapshot = request.session->initial_user_prompt;
    const std::shared_ptr<internal::AgentThreadState> session = request.session;

    json last_sink_json = json::object();
    bool sink_fired = false;
    bool merge_ok = true;

    CliAgentTerminalSinkOptions wrapped_sink = request.options.sink;
    std::function<void(const json&)> user_on_final_json = wrapped_sink.on_final_json;
    std::function<void(const std::shared_ptr<internal::AgentThreadState>&)> user_on_final_state =
        wrapped_sink.on_final_state;

    if (!wrapped_sink.on_final_json) {
        wrapped_sink.on_final_json = [](const json&) {};
    }

    wrapped_sink.on_final_json =
        [user_on_final_json, &last_sink_json, &sink_fired](const json& j) {
            sink_fired = true;
            last_sink_json = j;
            if (user_on_final_json) {
                user_on_final_json(j);
            }
        };

    wrapped_sink.on_final_state =
        [session, u_snapshot, user_on_final_state, &merge_ok](
            const std::shared_ptr<internal::AgentThreadState>& st_ptr) {
            if (!merge_react_session_state(*session, u_snapshot, st_ptr)) {
                merge_ok = false;
            }
            if (user_on_final_state) {
                user_on_final_state(st_ptr);
            }
        };

    workflow::GraphBuilder builder("react_cli_run");
    try {
        build_cli_agent_graph_with_terminal_sink(builder,
                                                 request.config,
                                                 request.deps,
                                                 session,
                                                 wrapped_sink,
                                                 request.options.loop_node_name,
                                                 request.options.graph_options);
    } catch (const std::exception& e) {
        wr.error_message = std::string("build graph: ") + e.what();
        return wr;
    }

    try {
        builder.run_async(executor).wait();
    } catch (const std::exception& e) {
        wr.error_message = std::string("run: ") + e.what();
        return wr;
    }

    if (!sink_fired) {
        wr.error_message = "run_react_cli_sync: terminal sink did not fire";
        return wr;
    }
    wr.outputs = last_sink_json;
    if (!merge_ok) {
        wr.success = false;
        wr.error_message = "state merge: history prefix mismatch";
        if (agent_log_level_debug()) {
            std::clog << "[GraphExecutor] state merge failed: history prefix mismatch (user_turn len="
                      << u_snapshot.size() << ")\n";
        }
        return wr;
    }

    wr.success = true;
    wr.error_message.reset();
    return wr;
}

std::future<WorkflowResult> GraphExecutor::run_react_cli_async(tf::Executor& executor,
                                                               ReactCliRunRequest request) {
    // Executor must outlive the future; see run_react_cli_async in graph_executor.hpp.
    return std::async(std::launch::async, [this, &executor, req = std::move(request)]() mutable {
        return this->run_react_cli_sync(executor, req);
    });
}

std::future<WorkflowResult> GraphExecutor::execute(const std::string& /*workflow_name*/) {
    throw std::logic_error("GraphExecutor::execute removed: use GraphExecutor::run_react_cli_sync");
}

std::shared_ptr<WorkflowTemplate> GraphExecutor::get_template(const std::string& name) const {
    std::lock_guard<std::mutex> lock(templates_mutex_);
    auto it = templates_.find(name);
    if (it == templates_.end()) {
        return nullptr;
    }
    return it->second;
}

std::vector<std::string> GraphExecutor::list_templates() const {
    std::lock_guard<std::mutex> lock(templates_mutex_);
    std::vector<std::string> out;
    out.reserve(templates_.size());
    for (const auto& kv : templates_) {
        out.push_back(kv.first);
    }
    return out;
}

} // namespace agent_framework
