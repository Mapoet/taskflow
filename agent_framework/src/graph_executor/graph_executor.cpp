/**
 * @file graph_executor.cpp
 * @brief GraphExecutor implementation (WP2.0 run_react_cli + session merge)
 */

#include <agent/graph_executor.hpp>

#include <agent/internal/agent_thread_state.hpp>
#include <agent/llm_client.hpp>
#include <agent/verifier_runner.hpp>
#include <agent/verifier_types.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <future>
#include <iostream>
#include <random>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace agent_framework {
namespace {

/** WP2.9 / WP2.7：允许 initial_user_prompt 为空，仅当首轮 pending 仅为 memory / model 审计类命令 */
bool react_cli_allow_empty_user_prompt(const internal::AgentThreadState& s) {
    if (s.pending_control_actions.empty()) {
        return false;
    }
    for (const auto& a : s.pending_control_actions) {
        if (a.command != "memory.clear" && a.command != "memory.compact" &&
            a.command != "model.set") {
            return false;
        }
    }
    return true;
}

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

void tolower_inplace(std::string& s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
}

enum class VerifierSwitchMode {
    Off,
    On,
    Sample
};

VerifierSwitchMode verifier_switch_from_env() {
    const char* e = std::getenv("AGENT_VERIFIER");
    if (!e || !*e) {
        return VerifierSwitchMode::Off;
    }
    std::string s(e);
    tolower_inplace(s);
    if (s == "on") {
        return VerifierSwitchMode::On;
    }
    if (s == "sample") {
        return VerifierSwitchMode::Sample;
    }
    return VerifierSwitchMode::Off;
}

bool verifier_sample_hit_from_env() {
    const char* e = std::getenv("AGENT_VERIFIER_SAMPLE_RATE");
    double rate = 0.1;
    if (e && *e) {
        rate = std::strtod(e, nullptr);
    }
    rate = std::max(0.0, std::min(1.0, rate));
    thread_local std::mt19937 rng{std::random_device{}()};
    std::bernoulli_distribution dist(rate);
    return dist(rng);
}

std::string utc_timestamp_iso_ms() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return oss.str();
}

json verifier_issues_to_json(const std::vector<VerifierIssue>& issues) {
    json arr = json::array();
    for (const auto& i : issues) {
        json one;
        one["code"] = i.code;
        one["severity"] = i.severity;
        one["detail"] = i.detail;
        arr.push_back(std::move(one));
    }
    return arr;
}

std::string verifier_log_outcome(VerifierGateKind k, bool skipped) {
    if (skipped) {
        return "skipped";
    }
    switch (k) {
        case VerifierGateKind::PublishPass:
            return "published";
        case VerifierGateKind::PublishPassThrough:
            return "published";
        case VerifierGateKind::RetryMain:
            return "retrying";
        case VerifierGateKind::Abort:
            return "aborted";
    }
    return "published";
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
        if (!react_cli_allow_empty_user_prompt(*r.session)) {
            throw std::invalid_argument("run_react_cli_sync: session->initial_user_prompt is empty");
        }
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
    const std::shared_ptr<internal::AgentThreadState>& next,
    MergeReactSessionMode mode) {
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
    if (mode == MergeReactSessionMode::FullUserTurn && !user_turn_snapshot.empty()) {
        Message user_msg;
        user_msg.role = "user";
        user_msg.content = user_turn_snapshot;
        user_msg.timestamp = std::time(nullptr);
        new_hist.push_back(std::move(user_msg));
    }
    new_hist.insert(new_hist.end(), delta.begin(), delta.end());

    session.history = std::move(new_hist);
    session.iteration = next->iteration;
    session.skill_prompt_cache = next->skill_prompt_cache;
    session.active_skill_id = next->active_skill_id;
    session.outbound_supervisor = next->outbound_supervisor;
    session.pending_injected_context = next->pending_injected_context;
    session.pending_control_actions = next->pending_control_actions;
    session.pending_input_violations = next->pending_input_violations;
    session.execution_context = next->execution_context;
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
    wr.exit_code = 0;
    try {
        validate_react_cli_request(request);
    } catch (const std::exception& e) {
        wr.error_message = e.what();
        return wr;
    }

    const std::string u_snapshot = request.session->initial_user_prompt;
    const std::shared_ptr<internal::AgentThreadState> session = request.session;
    session->verifier_retry_count = 0;

    const VerifierSwitchMode v_mode = verifier_switch_from_env();
    const bool verifier_sample_skip =
        (v_mode == VerifierSwitchMode::Sample) && !verifier_sample_hit_from_env();
    const bool verifier_should_run =
        (v_mode == VerifierSwitchMode::On) ||
        (v_mode == VerifierSwitchMode::Sample && !verifier_sample_skip);

    json last_sink_json = json::object();
    bool merge_ok = true;
    bool sink_fired = false;
    bool use_full_user_merge = true;
    const auto merge_mode_ptr = std::make_shared<bool>(true);

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
        [session, u_snapshot, user_on_final_state, &merge_ok, merge_mode_ptr](
            const std::shared_ptr<internal::AgentThreadState>& st_ptr) {
            const MergeReactSessionMode m = *merge_mode_ptr
                                                ? MergeReactSessionMode::FullUserTurn
                                                : MergeReactSessionMode::DeltaOnly;
            if (!merge_react_session_state(*session, u_snapshot, st_ptr, m)) {
                merge_ok = false;
            }
            if (user_on_final_state) {
                user_on_final_state(st_ptr);
            }
        };

    const int verifier_max_retries = verifier_max_retries_from_env();
    const int verifier_timeout_ms = verifier_timeout_ms_from_env();

    for (;;) {
        session->initial_user_prompt = u_snapshot;
        *merge_mode_ptr = use_full_user_merge;
        sink_fired = false;
        merge_ok = true;

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
        if (!merge_ok) {
            wr.success = false;
            wr.error_message = "state merge: history prefix mismatch";
            if (agent_log_level_debug()) {
                std::clog << "[GraphExecutor] state merge failed: history prefix mismatch (user_turn len="
                          << u_snapshot.size() << ")\n";
            }
            return wr;
        }
        use_full_user_merge = false;

        if (!verifier_should_run) {
            if (v_mode == VerifierSwitchMode::Sample && verifier_sample_skip) {
                std::string tid;
                std::string sid;
                if (session->execution_context) {
                    if (session->execution_context->task_id) {
                        tid = *session->execution_context->task_id;
                    }
                    if (session->execution_context->session_id) {
                        sid = *session->execution_context->session_id;
                    }
                }
                std::clog << "[verifier] task_id=" << (tid.empty() ? "-" : tid)
                          << " session_id=" << (sid.empty() ? "-" : sid) << " outcome=skipped\n";
            }
            wr.outputs = last_sink_json;
            wr.success = true;
            wr.error_message.reset();
            return wr;
        }

        if (request.options.on_verifier_event) {
            json pl;
            pl["component"] = "verifier";
            pl["task_id"] =
                session->execution_context && session->execution_context->task_id
                    ? json(*session->execution_context->task_id)
                    : json(nullptr);
            pl["session_id"] =
                session->execution_context && session->execution_context->session_id
                    ? json(*session->execution_context->session_id)
                    : json(nullptr);
            pl["ts"] = utc_timestamp_iso_ms();
            request.options.on_verifier_event("verifier_started", pl);
        }

        std::shared_ptr<LLMClient> verifier_llm = request.options.verifier_llm_override;
        if (!verifier_llm) {
            verifier_llm = make_verifier_llm_client_from_env();
        }

        const std::string draft = last_sink_json.value("final_answer", "");
        const std::size_t prompt_max = verifier_prompt_max_bytes_from_env();
        const std::string user_payload = build_verifier_user_json(*session, draft, prompt_max);
        VerifierRunOutput v_out = run_verifier_sync(*verifier_llm,
                                                    user_payload,
                                                    session->verifier_retry_count,
                                                    verifier_max_retries,
                                                    verifier_timeout_ms);
        const VerifierGateOutcome gate =
            apply_verifier_gate(v_out.parsed, session->verifier_retry_count, verifier_max_retries);

        std::string task_id_str;
        std::string session_id_str;
        if (session->execution_context) {
            if (session->execution_context->task_id) {
                task_id_str = *session->execution_context->task_id;
            }
            if (session->execution_context->session_id) {
                session_id_str = *session->execution_context->session_id;
            }
        }
        std::clog << "[verifier]"
                  << " task_id=" << (task_id_str.empty() ? "-" : task_id_str)
                  << " session_id=" << (session_id_str.empty() ? "-" : session_id_str)
                  << " ok=" << (v_out.parsed.ok ? "1" : "0")
                  << " action=" << gate.effective_action
                  << " issues_count=" << v_out.parsed.issues.size()
                  << " latency_ms=" << v_out.latency_ms
                  << " outcome=" << verifier_log_outcome(gate.kind, false) << "\n";

        if (request.options.on_verifier_event) {
            json pl;
            pl["component"] = "verifier";
            pl["task_id"] = task_id_str.empty() ? json(nullptr) : json(task_id_str);
            pl["session_id"] = session_id_str.empty() ? json(nullptr) : json(session_id_str);
            pl["ts"] = utc_timestamp_iso_ms();
            pl["ok"] = v_out.parsed.ok;
            pl["suggested_action"] = gate.effective_action;
            pl["issues_count"] = v_out.parsed.issues.size();
            pl["latency_ms"] = v_out.latency_ms;
            pl["verifier_ok"] = gate.verifier_ok;
            pl["gate_kind"] = static_cast<int>(gate.kind);
            request.options.on_verifier_event("verifier_completed", pl);
        }

        last_sink_json["verifier_ok"] = gate.verifier_ok;
        last_sink_json["suggested_action"] = gate.effective_action;
        last_sink_json["issues"] = verifier_issues_to_json(v_out.parsed.issues);

        if (gate.kind == VerifierGateKind::Abort) {
            wr.outputs = last_sink_json;
            wr.success = false;
            wr.exit_code = 4;
            std::string detail;
            if (!v_out.parsed.issues.empty()) {
                detail = v_out.parsed.issues.front().detail;
            }
            std::cerr << "[verifier] abort issues[0].code="
                      << (v_out.parsed.issues.empty() ? "" : v_out.parsed.issues.front().code)
                      << " detail=" << detail << "\n";
            wr.error_message = std::string("verifier_abort: ") + (detail.empty() ? "aborted" : detail);
            return wr;
        }

        if (gate.kind == VerifierGateKind::RetryMain) {
            Message fix;
            fix.role = "system";
            fix.content = build_verifier_fix_system_message(v_out.parsed.issues);
            fix.timestamp = std::time(nullptr);
            session->history.push_back(std::move(fix));
            ++session->verifier_retry_count;
            continue;
        }

        wr.outputs = last_sink_json;
        wr.success = true;
        wr.error_message.reset();
        return wr;
    }
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
