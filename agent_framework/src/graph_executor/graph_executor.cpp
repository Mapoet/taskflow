/**
 * @file graph_executor.cpp
 * @brief GraphExecutor implementation (WP2.0 run_react_cli + session merge)
 */

#include <agent/graph_executor/graph_executor.hpp>
#include <agent/observability/audit.hpp>
#include <agent/toolbus/tool_effect_journal.hpp>

#include <agent/internal/agent_thread_state.hpp>
#include <agent/llm_client/llm_client.hpp>
#include <agent/agent/task_state_machine.hpp>
#include <agent/agent/user_input_preprocessor.hpp>
#include <agent/agent/verifier_runner.hpp>
#include <agent/agent/verifier_types.hpp>

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
#include <unordered_set>

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

std::string stable_result_digest(const json& value) {
    const std::string bytes = value.dump();
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
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

ExecutionEventEmitter::ExecutionEventEmitter(ExecutionEventSink sink, std::string task_id,
                                             std::string session_id, std::string run_id)
    : sink_(std::move(sink)), task_id_(std::move(task_id)),
      session_id_(std::move(session_id)), run_id_(std::move(run_id)) {}

void ExecutionEventEmitter::emit(ExecutionEventType type, json payload,
                                 std::optional<std::string> child_id) noexcept {
    if (!sink_) return;
    try {
        ExecutionEvent event;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            event = {type, task_id_, session_id_, run_id_, std::move(child_id),
                     ++sequence_, utc_timestamp_iso_ms(), std::move(payload)};
        }
        sink_(event);
    } catch (...) {
        // Observability adapters cannot alter execution semantics.
    }
}

std::uint64_t ExecutionEventEmitter::sequence() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sequence_;
}

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
    if (mode == MergeReactSessionMode::ResumeFromCheckpoint && nh.size() <= old_hist.size()) {
        for (std::size_t i = 0; i < nh.size(); ++i) {
            if (!message_equal_for_merge(old_hist[i], nh[i])) {
                return false;
            }
        }
        session.iteration = std::max(session.iteration, next->iteration);
        session.last_memory_assembly_report = next->last_memory_assembly_report;
        session.memory_assembly_reports = next->memory_assembly_reports;
        session.last_memory_compaction_report = next->last_memory_compaction_report;
        session.memory_compaction_reports = next->memory_compaction_reports;
        session.last_memory_auto_compact_iteration = next->last_memory_auto_compact_iteration;
        session.last_memory_compaction_ts = next->last_memory_compaction_ts;
        session.last_error.clear();
        session.initial_user_prompt.clear();
        return true;
    }
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
    session.last_memory_assembly_report = next->last_memory_assembly_report;
    session.memory_assembly_reports = next->memory_assembly_reports;
    session.last_memory_compaction_report = next->last_memory_compaction_report;
    session.memory_compaction_reports = next->memory_compaction_reports;
    session.last_memory_auto_compact_iteration = next->last_memory_auto_compact_iteration;
    session.last_memory_compaction_ts = next->last_memory_compaction_ts;
    session.last_error = next->last_error;
    session.initial_user_prompt.clear();
    return true;
}

GraphExecutor::GraphExecutor() {
    register_react_cli_template();
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
    const internal::AgentThreadState session_snapshot = *session;
    session->last_error.clear();
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
        if (!session->last_error.empty()) {
            const std::string error = session->last_error;
            *session = session_snapshot;
            session->initial_user_prompt.clear();
            wr.success = false;
            wr.exit_code = 1;
            wr.error_message = error;
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

ExecutionResult GraphExecutor::execute_sync(tf::Executor& executor, ExecutionRequest request) {
    ExecutionResult result;
    const std::shared_ptr<WorkflowTemplate> workflow_template = get_template(request.template_id);
    if (!workflow_template) {
        result.error = "unknown workflow template: " + request.template_id;
        return result;
    }
    if (!request.session) {
        result.error = "execution session is null";
        return result;
    }

    std::string session_id;
    if (request.context.session_id) session_id = *request.context.session_id;
    if (session_id.empty() && request.session->execution_context &&
        request.session->execution_context->session_id) {
        session_id = *request.session->execution_context->session_id;
    }
    if (session_id.empty()) {
        result.error = "execution context session_id is required";
        return result;
    }
    result.session_id = session_id;

    std::uint64_t expected_revision = 0;
    SessionSnapshot loaded_snapshot;
    const std::string user_prompt = request.session->initial_user_prompt;
    if (request.session_store && request.options.persist_session) {
        loaded_snapshot = request.session_store->load_or_create(session_id);
        expected_revision = loaded_snapshot.revision;
        if (loaded_snapshot.revision != 0) *request.session = loaded_snapshot.state;
        request.session->initial_user_prompt = user_prompt;
    }
    auto committed_session = request.session;
    auto working_session = std::make_shared<internal::AgentThreadState>(*committed_session);
    working_session->memory_assembly_reports.clear();
    working_session->memory_compaction_reports.clear();
    request.session = working_session;
    request.context.session_id = session_id;
    request.session->execution_context = request.context;
    request.options.react.graph_options.task_control = request.control;
    auto prior_tool_observer = request.options.react.graph_options.tool_execution_observer;

    const std::string run_id = request.context.task_id.value_or(session_id) + ":" +
                               std::to_string(expected_revision + 1);
    // A caller-supplied journal spans retries/restarts.  Tool-call IDs are
    // generated by the agent loop and therefore form the idempotency portion;
    // session ID namespaces independently executing conversations.
    std::map<std::string, std::string> tool_effect_keys;
    std::vector<std::string> completed_tool_effect_keys;
    const auto public_event_sink = request.event_sink;
    const auto audit_sink = request.audit_sink;
    const std::size_t audit_attempt = expected_revision + 1;
    ExecutionEventEmitter emitter([public_event_sink, audit_sink, audit_attempt](const ExecutionEvent& event) {
        if (public_event_sink) public_event_sink(event);
        if (audit_sink) {
            AuditEvent audit;
            audit.timestamp = event.timestamp;
            audit.trace_id = event.run_id;
            audit.session_id = event.session_id;
            audit.task_id = event.task_id;
            audit.attempt = audit_attempt;
            audit.component = "graph_executor";
            audit.outcome = std::to_string(static_cast<int>(event.type));
            audit.sequence = event.sequence;
            audit.payload = redact_audit_payload(event.payload);
            audit.payload_digest = audit_payload_digest(audit.payload);
            audit_sink->write(audit);
        }
    }, request.context.task_id.value_or(""),
                                  session_id, run_id);
    auto emit = [&](ExecutionEventType type, json payload) {
        emitter.emit(type, std::move(payload));
    };
    request.options.react.graph_options.tool_execution_observer =
        [&, prior_tool_observer](const ToolExecutionEvent& event) {
            if (request.tool_effect_journal && !event.tool_call_id.empty()) {
                const std::string key = session_id + ":" + event.tool_call_id;
                if (event.phase == ToolExecutionPhase::Started) {
                    ToolEffectRecord effect;
                    effect.task_id = request.context.task_id.value_or("");
                    effect.session_id = session_id;
                    effect.tool_call_id = event.tool_call_id;
                    effect.idempotency_key = key;
                    effect.request_digest = stable_result_digest(event.arguments);
                    effect.attempt = expected_revision + 1;
                    // A duplicate start is an already-observed effect: do not
                    // overwrite its recovery evidence.
                    request.tool_effect_journal->start(std::move(effect));
                    tool_effect_keys[event.tool_call_id] = key;
                } else {
                    const auto it = tool_effect_keys.find(event.tool_call_id);
                    if (it != tool_effect_keys.end() &&
                        request.tool_effect_journal->complete(
                            it->second, stable_result_digest(event.result))) {
                        completed_tool_effect_keys.push_back(it->second);
                    }
                }
            }
            json payload{{"tool_name", event.tool_name},
                         {"tool_call_id", event.tool_call_id},
                         {"arguments", event.arguments}};
            if (event.phase == ToolExecutionPhase::Completed) payload["result"] = event.result;
            emit(event.phase == ToolExecutionPhase::Started ? ExecutionEventType::ToolStarted
                                                             : ExecutionEventType::ToolCompleted,
                 std::move(payload));
            if (prior_tool_observer) prior_tool_observer(event);
        };
    emit(ExecutionEventType::TaskStarted, json::object());

    if (!request.options.input_already_processed) {
        if (request.input_policy.tier_b_enabled && !request.input_policy.tier_b_llm) {
            result.error = "Tier B input policy enabled without a dedicated LLM client";
            return result;
        }
        PreprocessOptions preprocess;
        preprocess.toolbus = request.deps.toolbus;
        preprocess.enable_tier_b = request.input_policy.tier_b_enabled;
        preprocess.tier_b_llm = request.input_policy.tier_b_llm;
        preprocess.tier_b_timeout_ms = request.input_policy.tier_b_timeout_ms;
        preprocess.tier_b_max_calls = request.input_policy.tier_b_max_calls_per_request;
        preprocess.tier_b_reject_on_failure =
            request.input_policy.failure_mode == TierBFailureMode::Reject;
        UserInputPreprocessor pipeline(std::move(preprocess));
        ProcessedUserInput processed = pipeline.process(user_prompt, request.context);
        if (!processed.tier_a_violations.empty()) {
            result.error = "input_policy_violation: " + processed.tier_a_violations.front();
            emit(ExecutionEventType::TaskStatusChanged,
                 {{"component", "input_policy"}, {"outcome", "rejected"},
                  {"violations", processed.tier_a_violations}});
            return result;
        }
        apply_processed_to_agent_state(std::move(processed), request.context, *request.session);
        emit(ExecutionEventType::TaskStatusChanged,
             {{"component", "input_policy"}, {"outcome", "accepted"},
              {"tier_b_enabled", request.input_policy.tier_b_enabled}});
    }

    auto prior_verifier_event = request.options.react.on_verifier_event;
    request.options.react.on_verifier_event = [&, prior_verifier_event](std::string_view name, const json& payload) {
        emit(name == "verifier_started" ? ExecutionEventType::VerifierStarted
                                         : ExecutionEventType::VerifierCompleted, payload);
        if (prior_verifier_event) prior_verifier_event(name, payload);
    };

    WorkflowResult wr = workflow_template->execute(*this, executor, request);
    result.outputs = wr.outputs;
    result.exit_code = wr.exit_code;
    result.error = wr.error_message;
    for(const auto& assembly_report : request.session->memory_assembly_reports) {
        if(!assembly_report.is_object() || assembly_report.empty()) continue;
        emit(ExecutionEventType::MemoryAssembled, assembly_report);
        const auto decisions = assembly_report.value("decisions", json::array());
        if (decisions.is_array()) {
            for (const auto& decision : decisions) {
                const auto reason = decision.value("reason", "");
                if (reason == "budget_evicted" || reason == "budget_truncated" ||
                    reason == "slot_quota") {
                    emit(ExecutionEventType::MemoryEvicted, decision);
                }
            }
        }
    }
    request.session->memory_assembly_reports.clear();
    for(const auto& compaction_report : request.session->memory_compaction_reports) {
        if(compaction_report.is_object() && !compaction_report.empty())
            emit(ExecutionEventType::MemoryCompacted, compaction_report);
    }
    request.session->memory_compaction_reports.clear();
    if (request.control) {
        request.control->check_deadline_now();
    }
    const bool cancelled = request.control && request.control->is_cancel_requested();
    const bool deadline_exceeded = request.control && request.control->is_deadline_exceeded();
    if (!wr.success || cancelled || deadline_exceeded) {
        result.status = deadline_exceeded ? ExecutionTerminalStatus::DeadlineExceeded
                                         : cancelled ? ExecutionTerminalStatus::Cancelled
                                                     : ExecutionTerminalStatus::Failed;
        if (deadline_exceeded) result.error = "execution deadline exceeded";
        if (cancelled) result.error = "execution cancelled";
        emit(ExecutionEventType::ExecutionCompleted,
             {{"success", false}, {"exit_code", result.exit_code},
              {"cancelled", cancelled}, {"deadline_exceeded", deadline_exceeded}});
        return result;
    }

    if (request.session_store && request.options.persist_session) {
        SessionSnapshot next;
        next.session_id = session_id;
        next.revision = expected_revision;
        next.checkpoint_id = session_id + ":" + std::to_string(expected_revision + 1);
        next.state = *request.session;
        next.tool_commits = loaded_snapshot.tool_commits;
        next.child_tasks = loaded_snapshot.child_tasks;
        std::unordered_set<std::string> committed_ids;
        for (const auto& record : next.tool_commits) committed_ids.insert(record.tool_call_id);
        for (const auto& message : request.session->history) {
            if (message.role != "tool" || !message.tool_call_id || !message.tool_result ||
                committed_ids.contains(*message.tool_call_id)) {
                continue;
            }
            next.tool_commits.push_back(
                {*message.tool_call_id, 0, "completed", stable_result_digest(*message.tool_result)});
            committed_ids.insert(*message.tool_call_id);
        }
        SessionCommitResult committed = request.session_store->commit(next, expected_revision);
        if (committed.status != SessionCommitStatus::Committed) {
            result.status = committed.status == SessionCommitStatus::RevisionConflict
                ? ExecutionTerminalStatus::Conflict : ExecutionTerminalStatus::Failed;
            result.error = committed.error;
            result.exit_code = 1;
            return result;
        }
        result.committed_revision = committed.revision;
        result.checkpoint_id = next.checkpoint_id;
        if (request.tool_effect_journal) {
            for (const auto& key : completed_tool_effect_keys) {
                request.tool_effect_journal->commit(key);
            }
        }
        emit(ExecutionEventType::CheckpointCommitted,
             {{"revision", committed.revision}, {"checkpoint_id", next.checkpoint_id}});
    }
    // Without session persistence the completed workflow itself is the only
    // available commit boundary.  Keep the journal usable for ephemeral CLI
    // callers while durable callers commit strictly after SessionStore.
    if (request.tool_effect_journal &&
        !(request.session_store && request.options.persist_session)) {
        for (const auto& key : completed_tool_effect_keys) {
            request.tool_effect_journal->commit(key);
        }
    }
    *committed_session = *working_session;
    result.success = true;
    result.exit_code = 0;
    result.status = ExecutionTerminalStatus::Completed;
    emit(ExecutionEventType::ExecutionCompleted, {{"success", true}});
    return result;
}

std::future<ExecutionResult> GraphExecutor::execute_async(tf::Executor& executor,
                                                          ExecutionRequest request) {
    return std::async(std::launch::async, [this, &executor, req = std::move(request)]() mutable {
        return execute_sync(executor, std::move(req));
    });
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
