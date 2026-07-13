/**
 * @file agent_loop_node.cpp
 * @brief Agent 循环节点封装实现
 */

#include "node/agent_loop_node.hpp"
#include "node/llm_node.hpp"
#include "node/tool_call_node.hpp"
#include "node/knowledge_base_node.hpp"
#include "agent/internal/agent_thread_state.hpp"
#include "agent/internal/loop_io_keys.hpp"
#include "agent/skill_services.hpp"
#include "agent/context_budget.hpp"
#include "agent/task_state_machine.hpp"
#include "agent/toolbus.hpp"
#include "agent/user_input_preprocessor.hpp"
#include "agent/memory_compaction.hpp"
#include <agent/a2a/outbound_task_supervisor.hpp>

#include <algorithm>
#include <any>
#include <future>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <sstream>
#include <unordered_set>

namespace agent_framework {
namespace node {

namespace {

enum class LogLevel : int { Error = 0, Warn = 1, Info = 2, Debug = 3 };

bool env_truthy(const char* key) {
    const char* v = std::getenv(key);
    if (!v || !*v) {
        return false;
    }
    return v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T';
}

bool guard_repeat_tool_in_iteration_enabled() {
    // default ON
    if (std::getenv("AGENT_LOOP_GUARD_REPEAT_TOOL_IN_ITERATION") == nullptr) {
        return true;
    }
    return env_truthy("AGENT_LOOP_GUARD_REPEAT_TOOL_IN_ITERATION");
}

std::size_t guard_text_trunc() {
    const char* v = std::getenv("AGENT_LOOP_GUARD_TEXT_TRUNC");
    if (!v || !*v) {
        return 200;
    }
    const int n = std::atoi(v);
    if (n <= 0) {
        return 200;
    }
    return static_cast<std::size_t>(n);
}

std::string trunc_copy(const std::string& s, std::size_t cap) {
    if (s.size() <= cap) {
        return s;
    }
    return s.substr(0, cap) + "...";
}

LogLevel log_level_from_env() {
    const char* dbg = std::getenv("AGENT_TEST_AGENT_LOOP_DEBUG");
    if (dbg && std::string(dbg) != "0") {
        return LogLevel::Debug;
    }
    const char* e = std::getenv("AGENT_LOG_LEVEL");
    if (!e || !*e) {
        return LogLevel::Info;
    }
    std::string s(e);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (s == "debug") {
        return LogLevel::Debug;
    }
    if (s == "info") {
        return LogLevel::Info;
    }
    if (s == "warn" || s == "warning") {
        return LogLevel::Warn;
    }
    if (s == "error") {
        return LogLevel::Error;
    }
    return LogLevel::Info;
}

bool log_at_least(LogLevel need) {
    return static_cast<int>(log_level_from_env()) >= static_cast<int>(need);
}

bool pending_is_control_only_cli_turn(const std::vector<ControlAction>& actions) {
    if (actions.empty()) {
        return false;
    }
    for (const auto& a : actions) {
        if (a.command != "memory.clear" && a.command != "memory.compact" &&
            a.command != "model.set") {
            return false;
        }
    }
    return true;
}

} // namespace

std::pair<std::shared_ptr<workflow::LoopNode>, tf::Task>
AgentLoopNode::create(
    workflow::GraphBuilder& builder,
    const std::string& name,
    const AgentConfig& agent_config,
    std::shared_ptr<LLMClient> llm_client,
    std::shared_ptr<ToolBus> toolbus,
    std::shared_ptr<MemoryStore> memory_store,
    std::shared_ptr<VectorStore> vector_store,
    const std::vector<std::pair<std::string, std::string>>& input_specs,
    const std::vector<std::string>& output_keys,
    std::function<void(std::string_view)> stream_callback,
    std::shared_ptr<SkillServices> skills,
    std::shared_ptr<TaskControl> task_control,
    ToolExecutionObserver tool_execution_observer,
    SkillEventSink skill_event_sink
) {
    (void)memory_store;
    (void)vector_store;
    struct Shared {
        std::shared_ptr<internal::AgentThreadState> state;
        LLMOutput last_llm;
        std::string final_answer;
        bool is_final = false;
    };

    auto body_func = [agent_config, llm_client, toolbus, stream_callback, skills, task_control,
                      tool_execution_observer, skill_event_sink](
                         const workflow::ValueMap& inps,
                         const workflow::IterationContext&)
        -> std::unordered_map<std::string, std::any> {
        const char* dbg_env = std::getenv("AGENT_TEST_AGENT_LOOP_DEBUG");
        const bool dbg = dbg_env && std::string(dbg_env) != "0";
        auto notify_tool = [&](ToolExecutionEvent event) noexcept {
            if (!tool_execution_observer) return;
            try {
                tool_execution_observer(event);
            } catch (...) {
                // Observability must not alter agent execution semantics.
            }
        };
        ToolCallControl tool_control;
        if (task_control) {
            tool_control.cancellation_requested = [task_control]() {
                task_control->check_deadline_now();
                return task_control->is_cancel_requested() || task_control->is_deadline_exceeded();
            };
        }

        auto incoming = std::any_cast<std::shared_ptr<internal::AgentThreadState>>(
            inps.at(std::string(internal::kAgentState)));
        auto shared = std::make_shared<Shared>();
        shared->state = std::make_shared<internal::AgentThreadState>();
        if (incoming) {
            *shared->state = *incoming;
        }
        auto emit = [shared]() -> workflow::ValueMap {
            return {
                {std::string(internal::kFinalAnswer), std::any{shared->final_answer}},
                {std::string(internal::kNextAgentState), std::any{shared->state}},
                {std::string(internal::kLlmOutput), std::any{shared->last_llm}},
                {std::string(internal::kIsFinal), std::any{shared->is_final}}
            };
        };

        if (task_control) {
            task_control->check_deadline_now();
            if (task_control->is_deadline_exceeded() || task_control->is_cancel_requested()) {
                shared->is_final = true;
                shared->final_answer = task_control->is_cancel_requested() ? "[task] cancelled"
                                                                         : "[task] timeout";
                shared->last_llm.is_final = true;
                shared->last_llm.final_answer = shared->final_answer;
                shared->last_llm.tool_calls.clear();
                return emit();
            }
        }
        const int it = shared->state ? shared->state->iteration : 0;
        if (dbg) {
            std::cout << "[AgentLoop] iter=" << it << " history_size="
                      << (shared->state ? shared->state->history.size() : 0) << "\n";
            std::cout.flush();
        }

        const std::string user_query =
            std::any_cast<std::string>(inps.at(std::string(internal::kUserQuery)));
        const bool control_only_skip_llm =
            (it == 0 && user_query.empty() && shared->state &&
             pending_is_control_only_cli_turn(shared->state->pending_control_actions));

        if (skills && skills->registry && skills->loader && it == 0) {
            std::string user_for_match = user_query;
            if (user_for_match.empty()) {
                user_for_match = shared->state->initial_user_prompt;
            }
            if (const auto mid = skills->registry->match(user_for_match)) {
                const auto loaded = skills->loader->load_instructions(
                    *mid, skill_context_max_chars_from_env());
                if (loaded && !loaded->empty()) {
                    shared->state->skill_prompt_cache = *loaded;
                    shared->state->active_skill_id = *mid;
                } else {
                    shared->state->skill_prompt_cache.reset();
                    shared->state->active_skill_id.reset();
                }
            } else {
                shared->state->skill_prompt_cache.reset();
                shared->state->active_skill_id.reset();
            }
        }

        std::shared_ptr<const SkillPolicyEngine> active_skill_policy;
        if (skills && skills->registry && shared->state && shared->state->active_skill_id) {
            const auto entry = skills->registry->get(*shared->state->active_skill_id);
            const auto manifest = skills->registry->get_manifest(*shared->state->active_skill_id);
            if (entry && manifest) {
                SkillPermissionGrant grants;
                if (shared->state->execution_context) {
                    grants = shared->state->execution_context->skill_grants;
                }
                if (manifest->legacy_v0) {
                    // One-major-version compatibility: v0 allowed-tools remain their own task cap.
                    grants.tools = manifest->permissions.tools;
                    grants.network = manifest->permissions.network;
                    grants.environment = manifest->permissions.environment;
                    grants.filesystem_read = manifest->permissions.filesystem_read;
                    grants.filesystem_write = manifest->permissions.filesystem_write;
                    grants.secrets = manifest->permissions.secrets;
                }
                const auto package_root = entry->script_jail.value_or(entry->file_path.parent_path());
                active_skill_policy = std::make_shared<SkillPolicyEngine>(
                    manifest->permissions, grants, package_root);
                const std::string active_id = *shared->state->active_skill_id;
                const std::string task_id = shared->state->execution_context &&
                                                    shared->state->execution_context->task_id
                                                ? *shared->state->execution_context->task_id
                                                : std::string{};
                const std::string run_id = shared->state->execution_context &&
                                                   shared->state->execution_context->session_id
                                               ? *shared->state->execution_context->session_id
                                               : std::string{};
                tool_control.authorization =
                    [policy = active_skill_policy, skill_event_sink, active_id, task_id, run_id, it](
                        const std::string& tool_name, const json& arguments,
                        const ToolMeta& meta) -> std::optional<json> {
                    auto deny = [&](const SkillPolicyDecision& denied) -> std::optional<json> {
                        if (skill_event_sink) {
                            try {
                                skill_event_sink({SkillEventType::PermissionDenied,
                                                  kSkillPermissionDenied, active_id, {}, task_id,
                                                  run_id, 0, it,
                                                  {{"permission", skill_permission_kind_cstr(denied.kind)},
                                                   {"action", denied.action},
                                                   {"target", denied.target}}});
                            } catch (...) {
                            }
                        }
                        return SkillRuntime::permission_error(denied);
                    };
                    auto decision = policy->authorize_tool(tool_name);
                    if (!decision.allowed) return deny(decision);
                    for (const auto& target : meta.permission_targets) {
                        std::string value = target.static_target;
                        if (!target.argument.empty()) {
                            const auto found = arguments.find(target.argument);
                            if (found != arguments.end()) {
                                if (!found->is_string()) {
                                    SkillPolicyDecision invalid;
                                    invalid.kind = target.kind == ToolMeta::PermissionTargetKind::Network
                                                       ? SkillPermissionKind::Network
                                                       : (target.kind == ToolMeta::PermissionTargetKind::FilesystemWrite
                                                              ? SkillPermissionKind::FilesystemWrite
                                                              : SkillPermissionKind::FilesystemRead);
                                    invalid.action = "resolve_target";
                                    invalid.target = target.argument;
                                    invalid.reason = "permission target argument must be a string";
                                    return deny(invalid);
                                }
                                value = found->get<std::string>();
                            } else if (value.empty() && !target.base_path.empty()) {
                                value = target.base_path;
                            }
                        }
                        if (target.kind == ToolMeta::PermissionTargetKind::Network) {
                            decision = policy->authorize_network(value);
                        } else {
                            std::filesystem::path path(value);
                            if (!path.is_absolute() && !target.base_path.empty()) {
                                path = std::filesystem::path(target.base_path) / path;
                            }
                            decision = policy->authorize_filesystem(
                                path, target.kind == ToolMeta::PermissionTargetKind::FilesystemWrite);
                        }
                        if (!decision.allowed) return deny(decision);
                    }
                    return std::nullopt;
                };
                SkillInvocationContext invocation_context;
                invocation_context.control = task_control;
                invocation_context.grants = std::move(grants);
                invocation_context.event_sink = skill_event_sink;
                invocation_context.task_id = task_id;
                invocation_context.run_id = run_id;
                invocation_context.iteration = it;
                if (shared->state->execution_context) {
                    invocation_context.environment =
                        shared->state->execution_context->skill_environment;
                    invocation_context.secret_provider =
                        shared->state->execution_context->skill_secret_provider;
                    invocation_context.limits.max_input_bytes =
                        shared->state->execution_context->skill_max_input_bytes;
                    invocation_context.limits.max_output_bytes =
                        shared->state->execution_context->skill_max_output_bytes;
                }
                tool_control.skill_context =
                    std::make_shared<SkillInvocationContext>(std::move(invocation_context));
                tool_control.active_skill_id = active_id;
            }
        }

        std::string wp27_context_suffix;
        if (it == 0 && shared->state) {
            if (!shared->state->pending_input_violations.empty()) {
                shared->is_final = true;
                std::string msg = "[input_policy] violations:\n";
                for (const auto& v : shared->state->pending_input_violations) {
                    msg += v;
                    msg += '\n';
                }
                shared->final_answer = msg;
                shared->last_llm.is_final = true;
                shared->last_llm.final_answer = msg;
                shared->last_llm.tool_calls.clear();
                shared->state->pending_input_violations.clear();
                if (incoming) {
                    incoming->pending_input_violations.clear();
                }
                return emit();
            }
            dispatch_pending_control_actions(
                shared->state->pending_control_actions,
                shared->state->execution_context ? &*shared->state->execution_context : nullptr,
                shared->state.get(),
                &agent_config,
                llm_client.get());
            wp27_context_suffix = take_injected_blocks_as_llm_context(
                shared->state->pending_injected_context);

            // Preserve the public session contract: one-shot inputs are consumed from the
            // caller-owned state even though loop execution continues on an isolated snapshot.
            if (incoming) {
                incoming->pending_injected_context = shared->state->pending_injected_context;
                incoming->pending_control_actions = shared->state->pending_control_actions;
                incoming->pending_input_violations = shared->state->pending_input_violations;
            }
        }

        if (control_only_skip_llm) {
            if (dbg) {
                std::cout << "[AgentLoop] control_only turn: skip LLM (memory/model commands only)\n";
                std::cout.flush();
            }
            shared->is_final = true;
            shared->final_answer = "[memory] control command applied (no LLM turn).";
            shared->last_llm.is_final = true;
            shared->last_llm.final_answer = shared->final_answer;
            shared->last_llm.tool_calls.clear();
            shared->state->iteration += 1;
            MemoryCompactOptions mcopt;
            mcopt.agent_config = &agent_config;
            mcopt.llm_client = llm_client.get();
            maybe_auto_compact_memory(*shared->state, mcopt);
            return emit();
        }

        LLMInput llm_in;
        if (task_control) {
            llm_in.cancellation_requested = [task_control]() {
                task_control->check_deadline_now();
                return task_control->is_cancel_requested() ||
                       task_control->is_deadline_exceeded();
            };
        }
        llm_in.system_prompt =
            std::any_cast<std::string>(inps.at(std::string(internal::kSystemPrompt)));
        llm_in.user_prompt = user_query;
        llm_in.history = shared->state->history;
        if (it == 0 && !wp27_context_suffix.empty()) {
            llm_in.context = std::move(wp27_context_suffix);
        }
        std::string policy_ver = "wp27-v1";
        if (shared->state && shared->state->execution_context) {
            policy_ver = shared->state->execution_context->input_policy_version;
        }
        llm_in.extra_variables["input_policy_version"] = policy_ver;
        if (shared->state->skill_prompt_cache && !shared->state->skill_prompt_cache->empty()) {
            llm_in.skill_block = shared->state->skill_prompt_cache;
            llm_in.active_skill_id = shared->state->active_skill_id;
        }
        if (toolbus) {
            if (active_skill_policy) {
                llm_in.tools = toolbus->export_as_llm_tools(
                    [active_skill_policy](std::string_view name) {
                        return active_skill_policy->authorize_tool(name).allowed;
                    });
            } else {
                llm_in.tools = toolbus->export_as_llm_tools();
            }
        }
        if (shared->state && shared->state->outbound_supervisor) {
            std::size_t max_ev = 8;
            std::size_t max_b = 4096;
            if (const char* e = std::getenv("AGENT_A2A_SUBTASK_CONTEXT_MAX_EVENTS")) {
                if (e[0] != '\0') {
                    try {
                        const int v = std::stoi(std::string(e));
                        if (v >= 0) {
                            max_ev = static_cast<std::size_t>(v);
                        }
                    } catch (...) {
                    }
                }
            }
            if (const char* e = std::getenv("AGENT_A2A_SUBTASK_CONTEXT_BYTES")) {
                if (e[0] != '\0') {
                    try {
                        const int v = std::stoi(std::string(e));
                        if (v >= 0) {
                            max_b = static_cast<std::size_t>(v);
                        }
                    } catch (...) {
                    }
                }
            }
            llm_in.orchestrator_subtask_digest =
                shared->state->outbound_supervisor->format_digest_for_llm(max_ev, max_b);
        }

        // invoke (LLMClient will render using its configured PromptRenderer)
        if (dbg) {
            std::cout << "[AgentLoop] calling LLM...\n";
            std::cout.flush();
        }
        const auto t0 = std::chrono::steady_clock::now();
        LLMOutput llm_out;
        try {
            auto cancellable_stream = [stream_callback, task_control](std::string_view chunk) {
                if (task_control) {
                    task_control->check_deadline_now();
                    if (task_control->is_cancel_requested()) {
                        throw std::runtime_error("execution cancelled during LLM stream");
                    }
                    if (task_control->is_deadline_exceeded()) {
                        throw std::runtime_error("execution deadline exceeded during LLM stream");
                    }
                }
                if (stream_callback) stream_callback(chunk);
            };
            llm_out = llm_client->invoke(llm_in, "", std::move(cancellable_stream)).get();
        } catch (const std::exception& e) {
            if (dbg) {
                std::cout << "[AgentLoop] LLM call threw exception: " << e.what() << "\n";
                std::cout.flush();
            }
            // Set final state to break loop
            shared->is_final = true;
            shared->final_answer = std::string("[error] LLM call failed: ") + e.what();
            shared->last_llm.is_final = true;
            shared->last_llm.final_answer = shared->final_answer;
            shared->last_llm.tool_calls.clear();
            return emit();
        }
        const auto t1 = std::chrono::steady_clock::now();
        if (dbg) {
            const auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            std::cout << "[AgentLoop] LLM done in " << ms << "ms"
                      << " tool_calls=" << llm_out.tool_calls.size()
                      << " is_final=" << (llm_out.is_final ? "true" : "false")
                      << " final_answer_len=" << llm_out.final_answer.size() << "\n";
            std::cout.flush();
        }
        // A resumed attempt may receive the same stable tool_call_id again. Do not replay a
        // previously committed side effect; the matching tool result is already in history.
        if (!llm_out.tool_calls.empty()) {
            const bool had_tool_calls = true;
            std::unordered_set<std::string> committed_tool_calls;
            for (const auto& message : shared->state->history) {
                if (message.role == "tool" && message.tool_call_id &&
                    !message.tool_call_id->empty()) {
                    committed_tool_calls.insert(*message.tool_call_id);
                }
            }
            std::vector<CallSpec> pending_calls;
            pending_calls.reserve(llm_out.tool_calls.size());
            for (auto& call : llm_out.tool_calls) {
                if (!call.tool_call_id || call.tool_call_id->empty() ||
                    committed_tool_calls.find(*call.tool_call_id) == committed_tool_calls.end()) {
                    pending_calls.push_back(std::move(call));
                }
            }
            llm_out.tool_calls = std::move(pending_calls);
            if (had_tool_calls && llm_out.tool_calls.empty() && llm_out.final_answer.empty()) {
                // Tool routing wins over an inconsistent is_final flag. Advance once more so the
                // model can observe the already committed tool result and produce a final answer.
                llm_out.is_final = false;
            }
        }
        shared->last_llm = llm_out;

        // assistant message
        Message a;
        a.role = "assistant";
        a.timestamp = std::time(nullptr);
        if (!llm_out.tool_calls.empty()) {
            json j;
            j["tool_calls"] = json::array();
            for (const auto& c : llm_out.tool_calls) {
                json one;
                if (c.tool_call_id && !c.tool_call_id->empty()) {
                    one["id"] = *c.tool_call_id;
                }
                one["type"] = "function";
                one["function"] = json{{"name", c.name}, {"arguments", c.arguments.dump()}};
                j["tool_calls"].push_back(std::move(one));
            }
            a.content = j.dump();
        } else if (!llm_out.final_answer.empty()) {
            a.content = llm_out.final_answer;
        } else {
            a.content = llm_out.reasoning;
        }
        shared->state->history.push_back(std::move(a));

        // tools: WP2.1b orchestration (read parallel + cap) + repeat guard
        std::vector<CallSpec> calls = llm_out.tool_calls;
        if (static_cast<int>(calls.size()) > agent_config.max_tool_calls_per_iteration) {
            calls.resize(static_cast<std::size_t>(agent_config.max_tool_calls_per_iteration));
        }
        const bool guard_repeat_on = guard_repeat_tool_in_iteration_enabled();
        const std::size_t guard_trunc = guard_text_trunc();
        std::unordered_set<std::string> seen_calls;
        if (guard_repeat_on) {
            seen_calls.reserve(calls.size());
        }

        const ToolOrchestrationOptions orch_opts = resolve_tool_orchestration_options(agent_config);
        auto classify_side = [toolbus](std::string_view nm) -> ToolSideEffect {
            return toolbus->get_tool_meta(std::string(nm)).side_effect;
        };

        const ContextBudgetLimits ctx_budget_limits = ContextBudgetLimits::load(&agent_config);

        auto trigger_repeat_guard = [&](const CallSpec& c, const std::string& call_key) {
            const int iter = shared->state ? shared->state->iteration : 0;
            const std::string details = trunc_copy(call_key, guard_trunc);
            shared->is_final = true;
            shared->final_answer =
                "[guard] reason=repeat_tool_call_in_iteration iter=" + std::to_string(iter) +
                "\nDetected repeated tool call within the same iteration. "
                "Please avoid calling the same tool with identical arguments repeatedly. "
                "If inputs are missing, state the assumptions or request the missing fields.\n"
                "tool_call_key(truncated): " +
                details + "\n";
            shared->last_llm.is_final = true;
            shared->last_llm.final_answer = shared->final_answer;
            shared->last_llm.tool_calls.clear();
            if (log_at_least(LogLevel::Warn)) {
                std::clog << "[guard] reason=repeat_tool_call_in_iteration iter=" << iter
                          << " tool=" << c.name << "\n";
                std::clog.flush();
            }
        };

        auto append_tool_message = [&](const CallSpec& c, json result) {
            if (log_at_least(LogLevel::Info)) {
                std::clog << "\n[tool] name=" << c.name << " start\n";
                std::clog.flush();
            }
            apply_per_tool_result_budget(result, ctx_budget_limits, AfTruncationKind::tool_result);
            const bool warn_code =
                result.is_object() && result.contains("code") && result["code"].is_string();
            Message tm;
            tm.role = "tool";
            tm.tool_call_id = c.tool_call_id;
            tm.tool_name = c.name;
            tm.tool_result = std::move(result);
            tm.timestamp = std::time(nullptr);
            shared->state->history.push_back(std::move(tm));
            if (log_at_least(LogLevel::Info)) {
                std::clog << "[tool] name=" << c.name << " done\n";
                std::clog.flush();
            }
            if (warn_code && log_at_least(LogLevel::Warn)) {
                const Message& back = shared->state->history.back();
                std::clog << "[tool] name=" << c.name
                          << " warn code=" << (*back.tool_result)["code"].dump() << "\n";
                std::clog.flush();
            }
            if (dbg) {
                const Message& back = shared->state->history.back();
                std::cout << "[AgentLoop] tool " << c.name << " result=" << back.tool_result->dump()
                          << "\n";
                std::cout.flush();
            }
        };

        bool stop_tools = false;
        for (std::size_t i = 0; i < calls.size() && !stop_tools && toolbus;) {
            if (task_control) {
                task_control->check_deadline_now();
                if (task_control->is_cancel_requested() || task_control->is_deadline_exceeded()) {
                    stop_tools = true;
                    break;
                }
            }
            const ToolSideEffect se = classify_side(calls[i].name);
            const bool parallel_read_group =
                orch_opts.enable_parallel_reads && se == ToolSideEffect::ReadOnly;

            if (parallel_read_group) {
            std::size_t j = i + 1;
            while (j < calls.size() && classify_side(calls[j].name) == ToolSideEffect::ReadOnly) {
                ++j;
            }

            bool group_abort = false;
            for (std::size_t k = i; k < j; ++k) {
                const CallSpec& c = calls[k];
                const std::string call_key = c.name + "\n" + c.arguments.dump();
                if (guard_repeat_on) {
                    if (seen_calls.find(call_key) != seen_calls.end()) {
                        trigger_repeat_guard(c, call_key);
                        group_abort = true;
                        break;
                    }
                    seen_calls.insert(call_key);
                }
            }
            if (group_abort) {
                break;
            }

            std::vector<CallSpec> sub(calls.begin() + static_cast<std::ptrdiff_t>(i),
                                      calls.begin() + static_cast<std::ptrdiff_t>(j));
            for (const auto& c : sub) {
                if (dbg) {
                    std::cout << "[AgentLoop] calling tool (read group) " << c.name
                              << " args=" << c.arguments.dump() << "\n";
                    std::cout.flush();
                }
            }
            std::vector<json> part =
                execute_tool_calls_sequenced(toolbus, sub, orch_opts, classify_side,
                                              tool_execution_observer, tool_control);
            for (std::size_t t = 0; t < sub.size(); ++t) {
                append_tool_message(sub[t], std::move(part[t]));
            }
            i = j;
                continue;
            }

            if (orch_opts.enable_parallel_a2a_submits &&
                calls[i].name == a2a::kA2aToolSubmitTask) {
                std::size_t j2 = i + 1;
                while (j2 < calls.size() && calls[j2].name == a2a::kA2aToolSubmitTask) {
                    ++j2;
                }
                bool group_abort2 = false;
                for (std::size_t k = i; k < j2; ++k) {
                    const CallSpec& c = calls[k];
                    const std::string call_key = c.name + "\n" + c.arguments.dump();
                    if (guard_repeat_on) {
                        if (seen_calls.find(call_key) != seen_calls.end()) {
                            trigger_repeat_guard(c, call_key);
                            group_abort2 = true;
                            break;
                        }
                        seen_calls.insert(call_key);
                    }
                }
                if (group_abort2) {
                    break;
                }
                const int max_a2a = std::max(1, orch_opts.max_parallel_a2a_submits);
                const std::size_t glen2 = j2 - i;
                const std::size_t chunk2 = static_cast<std::size_t>(max_a2a);
                for (std::size_t chunk_start = 0; chunk_start < glen2 && !stop_tools;
                     chunk_start += chunk2) {
                    if (task_control) {
                        task_control->check_deadline_now();
                        if (task_control->is_cancel_requested() ||
                            task_control->is_deadline_exceeded()) {
                            stop_tools = true;
                            break;
                        }
                    }
                    const std::size_t chunk_end = std::min(chunk_start + chunk2, glen2);
                    std::vector<std::future<json>> futs;
                    futs.reserve(chunk_end - chunk_start);
                    for (std::size_t t = chunk_start; t < chunk_end; ++t) {
                        const std::size_t gi = i + t;
                        const CallSpec& c = calls[gi];
                        if (dbg) {
                            std::cout << "[AgentLoop] calling tool (a2a submit batch) " << c.name
                                      << " args=" << c.arguments.dump() << "\n";
                            std::cout.flush();
                        }
                        notify_tool({ToolExecutionPhase::Started, c.name,
                                     c.tool_call_id.value_or(""), c.arguments, {}});
                        futs.push_back(toolbus->call_tool(c.name, c.arguments, tool_control));
                    }
                    for (std::size_t u = 0; u < futs.size(); ++u) {
                        const CallSpec& c = calls[i + chunk_start + u];
                        json tool_result = futs[u].get();
                        notify_tool({ToolExecutionPhase::Completed, c.name,
                                     c.tool_call_id.value_or(""), c.arguments, tool_result});
                        append_tool_message(c, std::move(tool_result));
                    }
                }
                i = j2;
                continue;
            }

            {
                const CallSpec& c = calls[i];
                if (dbg) {
                    std::cout << "[AgentLoop] calling tool " << c.name << " args=" << c.arguments.dump()
                              << "\n";
                    std::cout.flush();
                }
                const std::string call_key = c.name + "\n" + c.arguments.dump();
                if (guard_repeat_on) {
                    if (seen_calls.find(call_key) != seen_calls.end()) {
                        trigger_repeat_guard(c, call_key);
                        break;
                    }
                    seen_calls.insert(call_key);
                }
                notify_tool({ToolExecutionPhase::Started, c.name, c.tool_call_id.value_or(""),
                             c.arguments, {}});
                json result = toolbus->call_tool(c.name, c.arguments, tool_control).get();
                notify_tool({ToolExecutionPhase::Completed, c.name, c.tool_call_id.value_or(""),
                             c.arguments, result});
                append_tool_message(c, std::move(result));
                ++i;
            }
        }

        shared->state->iteration += 1;
        if (!shared->is_final) {
            shared->is_final =
                llm_out.tool_calls.empty() && (llm_out.is_final || !llm_out.final_answer.empty());
            shared->final_answer = llm_out.final_answer;
        }

        if (shared->state) {
            MemoryCompactOptions mcopt;
            mcopt.agent_config = &agent_config;
            mcopt.llm_client = llm_client.get();
            maybe_auto_compact_memory(*shared->state, mcopt);
        }

        return emit();
    };

    auto condition_func = [task_control](const workflow::ValueMap& outputs,
                                        const workflow::IterationContext&)
        -> workflow::LoopDecision {
        if (task_control) {
            task_control->check_deadline_now();
            if (task_control->is_deadline_exceeded() || task_control->is_cancel_requested()) {
                return workflow::LoopDecision::Exit;
            }
        }
        const auto it = outputs.find(std::string(internal::kIsFinal));
        const bool is_final = it != outputs.end() && std::any_cast<bool>(it->second);
        return is_final ? workflow::LoopDecision::Exit : workflow::LoopDecision::Continue;
    };

    auto exit_func = [](const workflow::ValueMap& outputs,
                        const workflow::IterationContext&) -> workflow::ValueMap {
        const char* dbg_env = std::getenv("AGENT_TEST_AGENT_LOOP_DEBUG");
        const bool dbg = dbg_env && std::string(dbg_env) != "0";
        if (dbg) {
            std::cout << "[AgentLoop] exit_func called\n";
            std::cout.flush();
        }
        return outputs;
    };

    workflow::LoopOptions loop_options;
    loop_options.max_iterations = static_cast<std::size_t>(std::max(0, agent_config.max_iterations));
    loop_options.feedback = {
        {std::string(internal::kNextAgentState), std::string(internal::kAgentState)}
    };
    return builder.create_loop(
        name, input_specs, std::move(body_func), std::move(condition_func),
        std::move(exit_func), output_keys, std::move(loop_options));
}

void AgentLoopNode::build_loop_body(
    workflow::GraphBuilder& builder,
    const AgentConfig& agent_config,
    std::shared_ptr<LLMClient> llm_client,
    std::shared_ptr<ToolBus> toolbus,
    std::shared_ptr<MemoryStore> memory_store,
    std::shared_ptr<VectorStore> vector_store,
    const std::unordered_map<std::string, std::any>& inputs
) {
    (void)memory_store;
    (void)vector_store;
    // expose loop inputs as a source node
    auto [loop_in, loop_task] = builder.create_any_source("LoopInput", inputs);
    (void)loop_task;

    // LLM node: render + invoke_with_rendered_prompt
    const std::string model_name = agent_config.model_config.model_name;
    const std::string provider = "";  // default provider
    auto [llm_node, llm_task] = LLMNode::create(
        builder,
        "LLM",
        llm_client,
        std::make_shared<PromptRenderer>(),
        model_name,
        provider,
        {{"LoopInput", std::string(internal::kSystemPrompt)},
         {"LoopInput", std::string(internal::kUserQuery)},
         {"LoopInput", std::string(internal::kAgentState)}},
        nullptr);
    (void)llm_task;

    // ToolAggregator: WP2.1b orchestration (same as loop body minus repeat guard)
    const ContextBudgetLimits tool_agg_budget = ContextBudgetLimits::load(&agent_config);
    auto tool_agg = [toolbus, agent_config, tool_agg_budget](
                        const std::unordered_map<std::string, std::any>& inps)
        -> std::unordered_map<std::string, std::any> {
        const LLMOutput llm_out = std::any_cast<LLMOutput>(inps.at(std::string(internal::kLlmOutput)));
        std::vector<Message> tool_msgs;
        bool had_error = false;

        std::vector<CallSpec> calls = llm_out.tool_calls;
        if (static_cast<int>(calls.size()) > agent_config.max_tool_calls_per_iteration) {
            calls.resize(static_cast<std::size_t>(agent_config.max_tool_calls_per_iteration));
        }

        if (!toolbus || calls.empty()) {
            return {
                {std::string(internal::kToolMessages), std::any{tool_msgs}},
                {std::string(internal::kToolHadError), std::any{had_error}}
            };
        }

        const ToolOrchestrationOptions orch_opts = resolve_tool_orchestration_options(agent_config);
        auto classify_side = [toolbus](std::string_view nm) -> ToolSideEffect {
            return toolbus->get_tool_meta(std::string(nm)).side_effect;
        };
        std::vector<json> results(calls.size());
        std::vector<CallSpec> pending_calls;
        std::vector<std::size_t> pending_indexes;
        auto state = std::any_cast<std::shared_ptr<internal::AgentThreadState>>(
            inps.at(std::string(internal::kAgentState)));
        for (std::size_t i = 0; i < calls.size(); ++i) {
            bool restored = false;
            if (state && calls[i].tool_call_id) {
                for (auto it = state->history.rbegin(); it != state->history.rend(); ++it) {
                    if (it->role == "tool" && it->tool_call_id == calls[i].tool_call_id &&
                        it->tool_result) {
                        results[i] = *it->tool_result;
                        restored = true;
                        break;
                    }
                }
            }
            if (!restored) {
                pending_indexes.push_back(i);
                pending_calls.push_back(calls[i]);
            }
        }
        if (!pending_calls.empty()) {
            auto executed = execute_tool_calls_sequenced(
                toolbus, pending_calls, orch_opts, classify_side);
            for (std::size_t i = 0; i < executed.size(); ++i) {
                results[pending_indexes[i]] = std::move(executed[i]);
            }
        }
        for (std::size_t idx = 0; idx < calls.size(); ++idx) {
            json result = results[idx];
            apply_per_tool_result_budget(result, tool_agg_budget, AfTruncationKind::tool_result);
            Message m;
            m.role = "tool";
            m.content = "";
            m.tool_call_id = calls[idx].tool_call_id;
            m.tool_name = calls[idx].name;
            m.tool_result = std::move(result);
            m.timestamp = std::time(nullptr);
            if (result.is_object() && result.contains("code") && result["code"].is_string()) {
                had_error = true;
            }
            tool_msgs.push_back(std::move(m));
        }

        return {
            {std::string(internal::kToolMessages), std::any{tool_msgs}},
            {std::string(internal::kToolHadError), std::any{had_error}}
        };
    };

    auto [tool_node, tool_task] = builder.create_any_node(
        "ToolAggregator",
        {{"LLM", std::string(internal::kLlmOutput)},
         {"LoopInput", std::string(internal::kAgentState)}},
        tool_agg,
        {std::string(internal::kToolMessages), std::string(internal::kToolHadError)});
    (void)tool_task;

    // StateMerge: update history + iteration
    auto state_merge = [agent_config, llm_client](
                          const std::unordered_map<std::string, std::any>& inps)
        -> std::unordered_map<std::string, std::any> {
        auto st = std::any_cast<std::shared_ptr<internal::AgentThreadState>>(
            inps.at(std::string(internal::kAgentState)));
        const LLMOutput llm_out = std::any_cast<LLMOutput>(inps.at(std::string(internal::kLlmOutput)));
        const std::vector<Message> tool_msgs =
            std::any_cast<std::vector<Message>>(inps.at(std::string(internal::kToolMessages)));

        auto next = std::make_shared<internal::AgentThreadState>();
        if (st) {
            *next = *st;
        }
        // append assistant message
        Message a;
        a.role = "assistant";
        a.timestamp = std::time(nullptr);
        if (!llm_out.tool_calls.empty()) {
            json j;
            j["tool_calls"] = json::array();
            for (const auto& c : llm_out.tool_calls) {
                json one;
                one["type"] = "function";
                one["function"] = json{{"name", c.name}, {"arguments", c.arguments.dump()}};
                if (c.tool_call_id) {
                    one["id"] = *c.tool_call_id;
                }
                j["tool_calls"].push_back(std::move(one));
            }
            a.content = j.dump();
        } else if (!llm_out.final_answer.empty()) {
            a.content = llm_out.final_answer;
        } else {
            a.content = llm_out.reasoning;
        }
        next->history.push_back(std::move(a));
        for (const auto& tm : tool_msgs) {
            next->history.push_back(tm);
        }
        next->iteration += 1;

        MemoryCompactOptions mcopt;
        mcopt.agent_config = &agent_config;
        mcopt.llm_client = llm_client.get();
        maybe_auto_compact_memory(*next, mcopt);

        const bool is_final = llm_out.tool_calls.empty() && (llm_out.is_final || !llm_out.final_answer.empty());
        const std::string final_answer = llm_out.final_answer;

        return {
            {std::string(internal::kNextAgentState), std::any{next}},
            {std::string(internal::kIsFinal), std::any{is_final}},
            {std::string(internal::kFinalAnswer), std::any{final_answer}},
            {std::string(internal::kLlmOutput), std::any{llm_out}}
        };
    };

    builder.create_any_node(
        "StateMerge",
        {{"LoopInput", std::string(internal::kAgentState)},
         {"LLM", std::string(internal::kLlmOutput)},
         {"ToolAggregator", std::string(internal::kToolMessages)}},
        state_merge,
        {std::string(internal::kNextAgentState),
         std::string(internal::kIsFinal),
         std::string(internal::kFinalAnswer),
         std::string(internal::kLlmOutput)});
}

int AgentLoopNode::check_loop_condition(
    const std::unordered_map<std::string, std::any>& outputs,
    int iteration_count,
    int max_iterations
) {
    // 检查是否超过最大迭代次数
    if (iteration_count >= max_iterations) {
        return 1;  // 退出循环
    }
    
    // 检查是否已完成任务
    if (outputs.find("is_final") != outputs.end()) {
        bool is_final = std::any_cast<bool>(outputs.at("is_final"));
        if (is_final) {
            return 1;  // 退出循环
        }
    }
    
    return 0;  // 继续循环
    // 注意：workflow 的 create_loop_decl 的 condition_func 不直接支持 iteration_count
    // 实际实现中需要使用共享状态来跟踪迭代次数
}

void AgentLoopNode::build_exit_handler(
    workflow::GraphBuilder& builder,
    const std::unordered_map<std::string, std::any>& inputs
) {
    (void)builder;
    (void)inputs;
}

} // namespace node
} // namespace agent_framework
