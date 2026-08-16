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
#include <agent/skills/skill_services.hpp>
#include <agent/context_budget/context_budget.hpp>
#include <agent/memory/memory_assembly.hpp>
#include <agent/agent/task_state_machine.hpp>
#include <agent/toolbus/toolbus.hpp>
#include <agent/toolbus/tool_effect_journal.hpp>
#include <agent/agent/user_input_preprocessor.hpp>
#include <agent/agent/memory_compaction.hpp>
#include <agent/a2a/outbound_task_supervisor.hpp>
#include <agent/llm_runtime/runtime.hpp>

#include <algorithm>
#include <any>
#include <future>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <chrono>
#include <cctype>
#include <cmath>
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
    return utf8_safe_truncate(s, cap) + "...";
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

std::vector<CallSpec> bounded_tool_calls(std::vector<CallSpec> calls, int max_calls,
                                         int iteration) {
    const std::size_t cap = max_calls > 0 ? static_cast<std::size_t>(max_calls) : 0U;
    if (calls.size() > cap) {
        calls.resize(cap);
    }
    std::unordered_set<std::string> ids;
    ids.reserve(calls.size());
    for (std::size_t i = 0; i < calls.size(); ++i) {
        std::string id = calls[i].tool_call_id.value_or("");
        if (id.empty() || !ids.insert(id).second) {
            std::size_t salt = i;
            do {
                id = "af_call_" + std::to_string(iteration) + "_" + std::to_string(salt++);
            } while (!ids.insert(id).second);
            calls[i].tool_call_id = std::move(id);
        }
    }
    return calls;
}

MemoryCompactionProfile compaction_profile_from_config(const AgentConfig& config) {
    MemoryCompactionProfile profile;
    const auto string_value = [&](const char* key, std::string& target) {
        const auto found = config.extra_config.find(key);
        if(found != config.extra_config.end() && found->second.is_string())
            target = found->second.get<std::string>();
    };
    const auto size_value = [&](const char* key, std::size_t& target) {
        const auto found = config.extra_config.find(key);
        if(found != config.extra_config.end() && found->second.is_number_unsigned())
            target = found->second.get<std::size_t>();
    };
    string_value("MEMORY_COMPACTION_PROVIDER", profile.provider);
    string_value("MEMORY_COMPACTION_MODEL", profile.model);
    size_value("MEMORY_COMPACTION_MAX_INPUT_BYTES", profile.max_input_bytes);
    size_value("MEMORY_COMPACTION_MAX_OUTPUT_BYTES", profile.max_output_bytes);
    size_value("MEMORY_COMPACTION_MAX_INPUT_TOKENS", profile.max_input_tokens);
    size_value("MEMORY_COMPACTION_MAX_OUTPUT_TOKENS", profile.max_output_tokens);
    if(const auto found = config.extra_config.find("MEMORY_COMPACTION_TIMEOUT_MS");
       found != config.extra_config.end() && found->second.is_number_integer())
        profile.timeout_ms = found->second.get<int>();
    if(const auto found = config.extra_config.find("MEMORY_COMPACTION_MAX_RETRIES");
       found != config.extra_config.end() && found->second.is_number_integer())
        profile.max_retries = found->second.get<int>();
    if(const auto found = config.extra_config.find("MEMORY_COMPACTION_TEMPERATURE");
       found != config.extra_config.end() && found->second.is_number())
        profile.temperature = found->second.get<double>();
    if(const auto found = config.extra_config.find("MEMORY_COMPACTION_TOP_P");
       found != config.extra_config.end() && found->second.is_number())
        profile.top_p = found->second.get<double>();
    return profile;
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
    SkillEventSink skill_event_sink,
    std::function<void(std::string_view)> thinking_stream_callback,
    std::shared_ptr<LLMClient> memory_compaction_llm,
    std::shared_ptr<EncoderManager> encoder_manager,
    std::shared_ptr<llm_runtime::RoleRuntime> llm_role_runtime
) {
    (void)memory_store;
    if(static_cast<bool>(vector_store) != static_cast<bool>(encoder_manager))
        throw std::invalid_argument("AgentLoop RAG requires both VectorStore and EncoderManager");
    struct Shared {
        std::shared_ptr<internal::AgentThreadState> state;
        LLMOutput last_llm;
        std::string final_answer;
        std::string stop_reason;
        bool is_final = false;
    };

    auto body_func = [agent_config, llm_client, toolbus, stream_callback, thinking_stream_callback,
                      skills, task_control, memory_compaction_llm, vector_store, encoder_manager,
                      tool_execution_observer, skill_event_sink, llm_role_runtime](
                         const workflow::ValueMap& inps,
                         const workflow::IterationContext&)
        -> std::unordered_map<std::string, std::any> {
        const char* dbg_env = std::getenv("AGENT_TEST_AGENT_LOOP_DEBUG");
        const bool dbg = dbg_env && std::string(dbg_env) != "0";
        auto notify_tool = [&](ToolExecutionEvent event) {
            if (!tool_execution_observer) return;
            try {
                tool_execution_observer(event);
            } catch (const ToolEffectBlocked&) {
                throw;
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
        auto history_has_tool_receipt = [shared]() {
            if (!shared->state) return false;
            for (const auto& message : shared->state->history) {
                if (message.role == "tool" && message.tool_call_id &&
                    !message.tool_call_id->empty()) {
                    return true;
                }
            }
            return false;
        };
        auto emit = [shared, history_has_tool_receipt]() -> workflow::ValueMap {
            if(shared->is_final && shared->stop_reason.empty()) {
                if(shared->final_answer.rfind("[guard]",0)==0) shared->stop_reason="guard_stopped";
                else if(shared->final_answer.rfind("[error]",0)==0) shared->stop_reason="provider_failed";
                else if(shared->final_answer=="[task] cancelled") shared->stop_reason="cancelled";
                else if(shared->final_answer=="[task] timeout") shared->stop_reason="deadline_exceeded";
                else if(shared->final_answer.empty() && !history_has_tool_receipt())
                    shared->stop_reason="empty_delivery";
                else shared->stop_reason="model_turn_completed";
            }
            return {
                {std::string(internal::kFinalAnswer), std::any{shared->final_answer}},
                {std::string(internal::kNextAgentState), std::any{shared->state}},
                {std::string(internal::kLlmOutput), std::any{shared->last_llm}},
                {std::string(internal::kIsFinal), std::any{shared->is_final}},
                {std::string(internal::kModelStopReason), std::any{shared->stop_reason}}
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
            shared->stop_reason = task_control->is_cancel_requested() ? "cancelled" : "deadline_exceeded";
            shared->final_answer = "[memory] control command applied (no LLM turn).";
            shared->last_llm.is_final = true;
            shared->last_llm.final_answer = shared->final_answer;
            shared->last_llm.tool_calls.clear();
            shared->state->iteration += 1;
            MemoryCompactOptions mcopt;
            mcopt.agent_config = &agent_config;
            mcopt.sub_llm_client = memory_compaction_llm.get();
            mcopt.profile = compaction_profile_from_config(agent_config);
            if(task_control) mcopt.cancellation_requested = [task_control] {
                return task_control->is_cancel_requested() || task_control->is_deadline_exceeded();
            };
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
        const auto raw_system =
            std::any_cast<std::string>(inps.at(std::string(internal::kSystemPrompt)));
        MemoryAssemblyInput assembly_input;
        assembly_input.system.push_back(
            {MemorySlotKind::System, "system-prompt", 1000, 0, raw_system,
             json{{"origin", "agent_config"}}});
        assembly_input.task.push_back(
            {MemorySlotKind::Task, "current-user-task", 900, 0, user_query,
             json{{"origin", "user"}}});
        for(std::size_t index = 0; index < shared->state->history.size(); ++index) {
            const auto& message = shared->state->history[index];
            std::string text = message.content;
            const bool tool = message.role == "tool";
            if(tool && message.tool_result) {
                if(!text.empty()) text += "\n";
                text += message.tool_result->dump();
            }
            auto& target = tool ? assembly_input.tool : assembly_input.working;
            target.push_back({tool ? MemorySlotKind::Tool : MemorySlotKind::Working,
                              "history-" + std::to_string(index), tool ? 300 : 500, 0,
                              std::move(text), json{{"history_index", index}}});
        }
        if(it == 0 && !wp27_context_suffix.empty()) {
            assembly_input.retrieval.push_back(
                {MemorySlotKind::Retrieval, "input-policy-injected-context", 600, 0,
                 wp27_context_suffix, json{{"origin", "input_policy"}}});
        }
        if(vector_store && encoder_manager && !user_query.empty()) {
            int top_k = 5;
            std::string modality;
            MetadataFilter filter;
            if(const auto found = agent_config.extra_config.find("RAG_TOP_K");
               found != agent_config.extra_config.end() && found->second.is_number_integer())
                top_k = found->second.get<int>();
            if(const auto found = agent_config.extra_config.find("RAG_MODALITY");
               found != agent_config.extra_config.end() && found->second.is_string())
                modality = found->second.get<std::string>();
            if(const auto found = agent_config.extra_config.find("RAG_METADATA_FILTER");
               found != agent_config.extra_config.end() && found->second.is_object())
                for(const auto& [key, value] : found->second.items()) filter.emplace(key, value);
            const auto retrieval = retrieve_knowledge_base(
                user_query, vector_store, encoder_manager, top_k, modality, filter);
            for(const auto& result : retrieval.results) {
                assembly_input.retrieval.push_back(
                    {MemorySlotKind::Retrieval, "retrieval:" + result.doc_id, 600, 0,
                     "[source:" + result.doc_id + "] " + result.content,
                     json{{"citation_id", result.doc_id}, {"score", result.score},
                          {"metadata", result.metadata}}});
            }
        }
        if(shared->state->skill_prompt_cache && !shared->state->skill_prompt_cache->empty()) {
            assembly_input.skill.push_back(
                {MemorySlotKind::Skill, "active-skill", 700, 0,
                 *shared->state->skill_prompt_cache,
                 json{{"skill_id", shared->state->active_skill_id.value_or("unknown")}}});
        }
        if (shared->state->outbound_supervisor) {
            std::size_t max_events = 8;
            std::size_t max_bytes = 4096;
            if (const char* value = std::getenv("AGENT_A2A_SUBTASK_CONTEXT_MAX_EVENTS")) {
                try {
                    const int parsed = std::stoi(value);
                    if(parsed >= 0) max_events = static_cast<std::size_t>(parsed);
                } catch (...) {}
            }
            if (const char* value = std::getenv("AGENT_A2A_SUBTASK_CONTEXT_BYTES")) {
                try {
                    const int parsed = std::stoi(value);
                    if(parsed >= 0) max_bytes = static_cast<std::size_t>(parsed);
                } catch (...) {}
            }
            assembly_input.working.push_back(
                {MemorySlotKind::Working, "orchestrator-subtask-digest", 650, 0,
                 shared->state->outbound_supervisor->format_digest_for_llm(
                     max_events, max_bytes),
                 json{{"origin", "outbound_supervisor"}}});
        }
        const auto limits = ContextBudgetLimits::load(&agent_config);
        MemoryAssemblyPolicy assembly_policy;
        assembly_policy.hard_limit_bytes = limits.max_combined_prompt_attach_bytes;
        assembly_policy.soft_limit_bytes = limits.max_combined_prompt_attach_bytes
            ? limits.max_combined_prompt_attach_bytes - limits.max_combined_prompt_attach_bytes / 10
            : 0;
        assembly_policy.slot_quota_bytes[MemorySlotKind::Retrieval] = limits.max_injection_bytes;
        assembly_policy.slot_quota_bytes[MemorySlotKind::Skill] = limits.max_injection_bytes;
        assembly_policy.slot_quota_bytes[MemorySlotKind::Tool] = limits.max_tool_result_json_bytes;
        assembly_policy.minimum_retained_bytes[MemorySlotKind::System] = 1;
        assembly_policy.minimum_retained_bytes[MemorySlotKind::Task] = 1;
        if(const auto found = agent_config.extra_config.find("MEMORY_ASSEMBLY_HARD_TOKENS");
           found != agent_config.extra_config.end() && found->second.is_number_unsigned()) {
            assembly_policy.hard_limit_tokens = found->second.get<std::size_t>();
            assembly_policy.soft_limit_tokens = assembly_policy.hard_limit_tokens
                ? assembly_policy.hard_limit_tokens - assembly_policy.hard_limit_tokens / 10 : 0;
        }
        if(const auto found = agent_config.extra_config.find("MEMORY_ASSEMBLY_CHARS_PER_TOKEN");
           found != agent_config.extra_config.end() && found->second.is_number()) {
            const double chars_per_token = found->second.get<double>();
            if(chars_per_token > 0.0) {
                assembly_policy.token_estimator = [chars_per_token](std::string_view text) {
                    return static_cast<std::size_t>(
                        std::ceil(static_cast<double>(text.size()) / chars_per_token));
                };
            }
        }
        const auto assembled = assemble_memory(assembly_input, assembly_policy);
        shared->state->last_memory_assembly_report = assembled.report.to_json();
        shared->state->memory_assembly_reports.push_back(
            shared->state->last_memory_assembly_report);
        llm_in.extra_variables["memory_assembly_report"] =
            shared->state->last_memory_assembly_report.dump();
        std::map<std::string, std::string> kept;
        for(const auto& slot : assembled.slots) kept.emplace(slot.source_id, slot.text);
        llm_in.system_prompt = kept.contains("system-prompt") ? kept.at("system-prompt") : "";
        llm_in.user_prompt = kept.contains("current-user-task") ? kept.at("current-user-task") : "";
        for(const auto& slot : assembled.slots) {
            if(slot.kind != MemorySlotKind::Retrieval) continue;
            if(!llm_in.context.empty()) llm_in.context += "\n\n";
            llm_in.context += slot.text;
        }
        for(std::size_t index = 0; index < shared->state->history.size(); ++index) {
            const auto key = "history-" + std::to_string(index);
            const auto found = kept.find(key);
            if(found == kept.end()) continue;
            auto message = shared->state->history[index];
            std::string original = message.content;
            if(message.role == "tool" && message.tool_result) {
                if(!original.empty()) original += "\n";
                original += message.tool_result->dump();
            }
            if(found->second != original) {
                message.content = found->second;
                message.tool_result.reset();
            }
            llm_in.history.push_back(std::move(message));
        }
        std::string policy_ver = "wp27-v1";
        if (shared->state && shared->state->execution_context) {
            policy_ver = shared->state->execution_context->input_policy_version;
        }
        llm_in.extra_variables["input_policy_version"] = policy_ver;
        if (kept.contains("active-skill")) {
            llm_in.skill_block = kept.at("active-skill");
            llm_in.active_skill_id = shared->state->active_skill_id;
        }
        if (kept.contains("orchestrator-subtask-digest")) {
            llm_in.orchestrator_subtask_digest =
                kept.at("orchestrator-subtask-digest");
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
        // invoke (LLMClient will render using its configured PromptRenderer)
        if (dbg) {
            std::cout << "[AgentLoop] calling LLM...\n";
            std::cout.flush();
        }
        const auto t0 = std::chrono::steady_clock::now();
        LLMOutput llm_out;
        std::string stream_acc;
        try {
            for (int attempt = 0;; ++attempt) {
                stream_acc.clear();
                auto cancellable_stream =
                    [stream_callback, task_control, &stream_acc](std::string_view chunk) {
                    if (task_control) {
                        task_control->check_deadline_now();
                        if (task_control->is_cancel_requested()) {
                            throw std::runtime_error("execution cancelled during LLM stream");
                        }
                        if (task_control->is_deadline_exceeded()) {
                            throw std::runtime_error(
                                "execution deadline exceeded during LLM stream");
                        }
                    }
                    stream_acc.append(chunk.data(), chunk.size());
                    if (stream_callback) stream_callback(chunk);
                };
                auto cancellable_thinking =
                    [thinking_stream_callback, task_control](std::string_view chunk) {
                    if (task_control) {
                        task_control->check_deadline_now();
                        if (task_control->is_cancel_requested())
                            throw std::runtime_error("execution cancelled during LLM stream");
                        if (task_control->is_deadline_exceeded())
                            throw std::runtime_error(
                                "execution deadline exceeded during LLM stream");
                    }
                    if (thinking_stream_callback) thinking_stream_callback(chunk);
                };
                if(llm_role_runtime) {
                    llm_runtime::RoleInvocationRequest role_request;
                    if(!shared->state || !shared->state->execution_context)
                        throw std::runtime_error("LLM Role Runtime requires an ExecutionContext");
                    const auto& execution = *shared->state->execution_context;
                    role_request.metadata.identity.tenant_id = execution.tenant_id;
                    role_request.metadata.identity.task_id = execution.task_id.value_or("");
                    role_request.trace_id = execution.trace_id;
                    const auto profile_id = agent_config.extra_config.find("LLM_ROLE_PROFILE_ID");
                    const auto profile_revision =
                        agent_config.extra_config.find("LLM_ROLE_PROFILE_REVISION");
                    if(profile_id == agent_config.extra_config.end() ||
                       !profile_id->second.is_string() ||
                       profile_revision == agent_config.extra_config.end() ||
                       !profile_revision->second.is_string())
                        throw std::runtime_error(
                            "LLM Role Runtime requires pinned profile id and revision");
                    role_request.profile_id = profile_id->second.get<std::string>();
                    role_request.profile_revision = profile_revision->second.get<std::string>();
                    role_request.policy_revision = execution.input_policy_version;
                    role_request.prompt_variables = llm_in.extra_variables;
                    role_request.prompt_variables["caller_system_prompt"] = llm_in.system_prompt;
                    role_request.prompt_variables["caller_user_prompt"] = llm_in.user_prompt;
                    role_request.input = llm_in;
                    const auto assign_string_config = [&](const char* key, std::string& target) {
                        const auto found = agent_config.extra_config.find(key);
                        if(found != agent_config.extra_config.end() && found->second.is_string())
                            target = found->second.get<std::string>();
                    };
                    assign_string_config("LLM_MEMORY_SNAPSHOT_ID",
                                        role_request.memory_view.snapshot_id);
                    assign_string_config("LLM_MEMORY_VIEW_PROFILE",
                                        role_request.memory_view.profile);
                    assign_string_config("LLM_MEMORY_VIEW_DIGEST",
                                        role_request.memory_view.view_digest);
                    assign_string_config("LLM_REQUIRED_REGION", role_request.required_region);
                    if(const auto found = agent_config.extra_config.find("LLM_GRANTED_CAPABILITIES");
                       found != agent_config.extra_config.end() && found->second.is_array()) {
                        for(const auto& capability : found->second) {
                            if(capability.is_string())
                                role_request.granted_capabilities.push_back(
                                    capability.get<std::string>());
                        }
                    }
                    for(const auto& tool : llm_in.tools) {
                        role_request.granted_capabilities.push_back(tool.name);
                        role_request.granted_capabilities.push_back("tool:" + tool.name);
                    }
                    role_request.estimated_input_tokens = static_cast<std::uint64_t>(
                        (llm_in.system_prompt.size() + llm_in.user_prompt.size() +
                         llm_in.context.size()) / 4U);
                    auto role_result = llm_role_runtime->invoke(
                        std::move(role_request), std::move(cancellable_stream),
                        std::move(cancellable_thinking));
                    if(!role_result.ok)
                        throw std::runtime_error("LLM Role Runtime: " + role_result.error_code +
                                                 ": " + role_result.error_message);
                    llm_out = std::move(role_result.output);
                } else {
                    llm_out = llm_client->invoke_channels(
                        llm_in, "", std::move(cancellable_stream),
                        std::move(cancellable_thinking)).get();
                }
                const std::size_t stream_len = stream_acc.size();
                if (llm_out.final_answer.empty() && !stream_acc.empty())
                    llm_out.final_answer = std::move(stream_acc);
                const bool empty_delivery =
                    llm_out.final_answer.empty() && llm_out.tool_calls.empty();
                if (dbg || empty_delivery) {
                    std::clog << "[AgentLoop] provider response"
                              << " attempt=" << attempt
                              << " final_answer_len=" << llm_out.final_answer.size()
                              << " stream_len=" << stream_len
                              << " tool_calls=" << llm_out.tool_calls.size()
                              << " is_final=" << (llm_out.is_final ? "true" : "false") << "\n";
                    std::clog.flush();
                }
                if (!empty_delivery || attempt >= 1)
                    break;
                if (log_at_least(LogLevel::Warn)) {
                    std::clog << "[AgentLoop] empty provider response, retrying once\n";
                    std::clog.flush();
                }
            }
        } catch (const std::exception& e) {
            if (dbg) {
                std::cout << "[AgentLoop] LLM call threw exception: " << e.what() << "\n";
                std::cout.flush();
            }
            // Set final state to break loop
            shared->is_final = true;
            shared->stop_reason = "provider_failed";
            shared->final_answer = std::string("[error] LLM call failed: ") + e.what();
            shared->last_llm.is_final = true;
            shared->last_llm.final_answer = shared->final_answer;
            shared->last_llm.tool_calls.clear();
            shared->state->last_error = shared->final_answer;
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
        llm_out.tool_calls = bounded_tool_calls(
            std::move(llm_out.tool_calls), agent_config.max_tool_calls_per_iteration, it);
        shared->last_llm = llm_out;

        auto make_assistant_message = [](const LLMOutput& output,
                                         const std::vector<CallSpec>& tool_calls) {
            Message a;
            a.role = "assistant";
            a.timestamp = std::time(nullptr);
            if (!tool_calls.empty()) {
                json j;
                j["tool_calls"] = json::array();
                for (const auto& c : tool_calls) {
                    json one;
                    if (c.tool_call_id && !c.tool_call_id->empty()) {
                        one["id"] = *c.tool_call_id;
                    }
                    one["type"] = "function";
                    one["function"] =
                        json{{"name", c.name}, {"arguments", c.arguments.dump()}};
                    j["tool_calls"].push_back(std::move(one));
                }
                a.content = j.dump();
            } else if (!output.final_answer.empty()) {
                a.content = output.final_answer;
            } else {
                a.content = output.reasoning;
            }
            return a;
        };

        const std::size_t tool_group_insert_pos = shared->state->history.size();
        if (llm_out.tool_calls.empty()) {
            shared->state->history.push_back(make_assistant_message(llm_out, {}));
        }

        // tools: WP2.1b orchestration (read parallel + cap) + repeat guard
        std::vector<CallSpec> calls = llm_out.tool_calls;
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
            shared->stop_reason = "guard_stopped";
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
                //std::clog << "\n[tool] name=" << c.name << " start\n";
                //std::clog.flush();
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
                //std::clog << "[tool] name=" << c.name << " done\n";
                //std::clog.flush();
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

        if (!calls.empty()) {
            std::unordered_set<std::string> completed_ids;
            for (std::size_t h = tool_group_insert_pos; h < shared->state->history.size(); ++h) {
                const Message& message = shared->state->history[h];
                if (message.role == "tool" && message.tool_call_id) {
                    completed_ids.insert(*message.tool_call_id);
                }
            }
            std::vector<CallSpec> completed_calls;
            completed_calls.reserve(completed_ids.size());
            for (const auto& call : calls) {
                if (call.tool_call_id && completed_ids.contains(*call.tool_call_id)) {
                    completed_calls.push_back(call);
                }
            }
            if (!completed_calls.empty()) {
                shared->state->history.insert(
                    shared->state->history.begin() +
                        static_cast<std::ptrdiff_t>(tool_group_insert_pos),
                    make_assistant_message(llm_out, completed_calls));
            } else if (shared->is_final && !shared->final_answer.empty()) {
                LLMOutput terminal = llm_out;
                terminal.final_answer = shared->final_answer;
                terminal.reasoning.clear();
                shared->state->history.push_back(make_assistant_message(terminal, {}));
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
            mcopt.sub_llm_client = memory_compaction_llm.get();
            mcopt.profile = compaction_profile_from_config(agent_config);
            if(task_control) mcopt.cancellation_requested = [task_control] {
                return task_control->is_cancel_requested() || task_control->is_deadline_exceeded();
            };
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
