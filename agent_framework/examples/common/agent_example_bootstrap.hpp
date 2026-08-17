#pragma once

#include <agent/agent_server/agent_server.hpp>
#include <agent/llm_client/llm_client.hpp>
#include <agent/prompt_renderer/prompt_renderer.hpp>
#include <agent/skills/skill_runtime.hpp>
#include <agent/skills/skill_services.hpp>
#include <agent/skills/skill_script_tool.hpp>
#include <agent/toolbus/draw_tools.hpp>
#include <agent/toolbus/expr_tools.hpp>
#include <agent/toolbus/fs_tools.hpp>
#include <agent/toolbus/process_tools.hpp>
#include <agent/toolbus/toolbus.hpp>
#include <agent/toolbus/web_tools.hpp>
#include <agent/conversation/production_bridge.hpp>
#include <agent/conversation/conversation_engine.hpp>
#include <agent/conversation/graph_turn_adapter.hpp>
#include <agent/conversation/harness_supported_runtime.hpp>
#include <agent/conversation/harness_turn_adapter.hpp>
#include <agent/conversation/task_registry.hpp>
#include <agent/conversation/task_classifier.hpp>
#include <agent/conversation/task_clarification_coordinator.hpp>
#include <agent/conversation/task_control_service.hpp>
#include <agent/conversation/task_command_service.hpp>
#include <agent/conversation/task_orchestrator.hpp>
#include <agent/harness/store.hpp>
#include <agent/runtime/production_live_runtime.hpp>
#include <agent/agent_template/runner.hpp>
#include <agent/ui/live_operations_projection.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace agent_framework::example {

struct BootstrapOptions {
    std::filesystem::path skills_root;
    std::string cursor_mcp_config;
    bool use_cursor_skill_roots = true;
    bool import_cursor_mcp = true;
    bool verbose = false;
    std::vector<std::string> skip_mcp_services;
};

struct BootstrapResult {
    std::shared_ptr<SkillServices> skills;
    std::size_t mcp_services = 0;
    std::vector<std::string> registered_mcp_services;
    std::vector<std::string> skipped_mcp_services;
    std::vector<std::string> diagnostics;
};

struct SkillUiStatus {
    bool enabled = false;
    std::size_t count = 0;
    std::uint64_t generation = 0;
    std::size_t diagnostics = 0;
    std::size_t errors = 0;
    std::string root;
    std::string active = "-";
};

/**
 * @brief All interactive/live examples share this single construction path.
 *
 * Keep UI and server adapters deliberately thin: option parsing belongs to an
 * executable, while provider defaults, local tools, MCP, Skills, built-ins,
 * and the common AgentConfig belong here.
 */
struct LiveRuntimeOptions {
    std::string agent_name = "agent-demo";
    std::filesystem::path skills_root;
    std::string cursor_mcp_config;
    std::vector<std::string> skip_mcp_services;
    std::string provider;
    std::string model;
    int max_iterations = -1;
    bool use_cursor_skill_roots = true;
    bool import_cursor_mcp = true;
    bool enable_skills = true;
    bool enable_tier_b = false;
    bool verbose = false;
    // Production deployments provide the complete validated composition here.
    // Demo/test deployments leave it empty and use the interactive harness.
    std::shared_ptr<runtime::ProductionRuntimeResources> production_resources;
    std::shared_ptr<conversation::TaskCommandPolicy> task_command_policy;
    std::function<std::optional<conversation::TaskCommandPrincipal>(
        const conversation::ConversationIdentity&)> task_principal_resolver;
    std::function<void(const nlohmann::json&, std::uint64_t)> task_action_observer;
};

struct LiveRuntime {
    std::shared_ptr<LLMClient> llm;
    std::shared_ptr<LLMClient> memory_compaction_llm;
    std::shared_ptr<ToolBus> toolbus;
    std::shared_ptr<SkillServices> skills;
    std::shared_ptr<agent_template::SkillRunnerRegistry> skill_runners;
    AgentConfig config;
    InputPolicyConfig input_policy;
    ExecutionTrustProfile trust_profile{ExecutionTrustProfile::Demo};
    conversation::TaskExecutionProfile task_profile{
        conversation::TaskExecutionProfile::Conversation};
    // nullopt means no deployment override; an explicit "conversation" is a real override.
    std::optional<conversation::TaskExecutionProfile> configured_task_profile;
    BootstrapResult bootstrap;
    conversation::HarnessSupportedTurnRuntime::Executor harness_turn_executor;
    conversation::HarnessSupportedTurnRuntime::Executor long_task_executor;
    std::shared_ptr<conversation::TaskClassifier> task_classifier;
    std::shared_ptr<conversation::TaskControlService> task_control_service;
    std::shared_ptr<conversation::TaskCommandPolicy> task_command_policy;
    std::function<std::optional<conversation::TaskCommandPrincipal>(
        const conversation::ConversationIdentity&)> task_principal_resolver;
    std::function<void(const nlohmann::json&, std::uint64_t)> task_action_observer;
    std::shared_ptr<runtime::ProductionLiveRuntime> production_runtime;
    bool harness_ready{false};
    bool explicit_legacy_fallback{false};
    nlohmann::json composition_report = nlohmann::json::object();
};

inline void bind_task_action_observer(
    LiveRuntime& runtime, const std::shared_ptr<LiveOperationsProjection>& projection) {
    std::weak_ptr<LiveOperationsProjection> weak = projection;
    runtime.task_action_observer = [weak](const nlohmann::json& value,
                                          std::uint64_t revision) {
        if(!value.is_array()) return;
        std::vector<OperationsTaskAction> actions;
        actions.reserve(value.size());
        for(const auto& item : value) {
            if(!item.is_object()) continue;
            OperationsTaskAction action;
            action.command = item.value("command", "");
            action.required_scope = item.value("required_scope", "");
            action.enabled = item.value("enabled", false);
            action.reason = item.value("reason", "");
            if(!action.command.empty()) actions.push_back(std::move(action));
        }
        if(auto target = weak.lock())
            target->observe_task_actions(std::move(actions), revision);
    };
}

using GraphTurnCallback = std::function<WorkflowResult()>;

inline conversation::TurnResult run_conversation_turn(
    const LiveRuntime& runtime, std::string_view agent_name, std::string input,
    GraphTurnCallback graph, conversation::RuntimeEventSink event_sink = {},
    std::optional<std::uint64_t> expected_task_revision = std::nullopt) {
    if(!runtime.harness_ready && !runtime.explicit_legacy_fallback)
        throw std::runtime_error(
            "harness_unavailable_and_fallback_not_explicit: set AGENT_LEGACY_REACT_FALLBACK=1 "
            "only for non-production compatibility");
    if(!graph) throw std::invalid_argument("model/tool execution callback required");
    const char* configured_db = std::getenv("AGENT_CONVERSATION_DB");
    std::filesystem::path database = configured_db && *configured_db
        ? std::filesystem::path(configured_db)
        : std::filesystem::path(".agent-framework") /
              (std::string(agent_name) + "-conversation.sqlite3");
    const char* configured_tenant = std::getenv("AGENT_TENANT_ID");
    const char* configured_conversation = std::getenv("AGENT_CONVERSATION_ID");
    conversation::ConversationIdentity identity{
        configured_tenant && *configured_tenant ? configured_tenant : "local",
        configured_conversation && *configured_conversation
            ? configured_conversation : std::string(agent_name)};
    static std::atomic<std::uint64_t> serial{0};
    const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    conversation::TurnRequest request{
        identity,
        std::string(agent_name) + ":" + std::to_string(now) + ":" +
            std::to_string(serial.fetch_add(1, std::memory_order_relaxed)),
        std::move(input), runtime.task_profile,
        static_cast<std::uint64_t>(std::max(1, runtime.config.max_iterations))};
    conversation::SQLiteConversationStore store(database.string());
    conversation::SQLiteTaskRegistry task_registry(database.string());
    conversation::SQLiteTaskProfileClarificationStore clarification_store(database.string());
    const char* configured_task = std::getenv("AGENT_TASK_ID");
    const char* configured_run = std::getenv("AGENT_RUN_ID");
    auto active_task = task_registry.active(identity);
    auto pending_clarification = clarification_store.pending(identity);
    auto intent = pending_clarification
        ? pending_clarification->task_intent
        : conversation::classify_task_input(request.input, active_task.has_value());
    const bool control_only = intent == conversation::TaskInputIntent::StatusQuery ||
        intent == conversation::TaskInputIntent::CancelTask ||
        intent == conversation::TaskInputIntent::SuspendTask;
    const bool force_new = intent == conversation::TaskInputIntent::StartNewTask;
    if(pending_clarification) {
        request.task_id=pending_clarification->task_id;
        request.run_id=pending_clarification->run_id;
    } else if(configured_task && *configured_task) {
        request.task_id = configured_task;
        auto selected = task_registry.load(identity, request.task_id);
        if(selected) active_task = std::move(selected);
    } else if(active_task && !force_new) {
        request.task_id = active_task->task_id;
    } else {
        request.task_id = std::string(agent_name) + ":task:" +
            std::to_string(now) + ":" +
            std::to_string(serial.fetch_add(1, std::memory_order_relaxed));
        active_task.reset();
    }
    // A non-control turn creates a new executable requirement revision and
    // therefore needs a new Run identity.  Reusing current_run_id here made
    // the second interactive turn collide with task_run_links' primary key.
    // Control commands observe/mutate the current Run and deliberately retain
    // it.  An explicit AGENT_RUN_ID remains an operator-owned idempotency key.
    if(!pending_clarification)
        request.run_id = conversation::select_task_run_id(
            configured_run && *configured_run ? configured_run : "",
            active_task ? active_task->current_run_id : "", control_only,
            request.turn_id);
    std::optional<conversation::TaskClassification> classification;
    bool needs_clarification=false;
    if(!pending_clarification && runtime.task_classifier && !control_only) {
        classification=runtime.task_classifier->classify(
            request.input,active_task.has_value());
        *classification=conversation::apply_task_routing_policy(
            std::move(*classification),runtime.configured_task_profile,runtime.trust_profile);
        if(!*classification)
            *classification=conversation::deterministic_task_classification(request.input);
        request.profile=classification->profile;
        needs_clarification=classification->requires_confirmation;
    }
    if(expected_task_revision &&
       (!active_task || active_task->revision != *expected_task_revision))
        throw std::runtime_error("task_command_stale_revision");
    if(intent == conversation::TaskInputIntent::StatusQuery) {
        conversation::TaskCommandService commands(task_registry,
            runtime.task_control_service.get(),runtime.task_command_policy.get());
        conversation::TaskCommandRequest command{
            conversation::TaskCommandKind::Status, request};
        command.expected_task_revision=expected_task_revision;
        if(runtime.task_principal_resolver)
            command.principal=runtime.task_principal_resolver(identity);
        const auto status = commands.execute(command);
        if(status.ok && runtime.task_action_observer &&
           status.payload.contains("actions"))
            runtime.task_action_observer(status.payload["actions"],status.task_revision);
        const std::string response = status.ok
            ? status.payload.dump(2)
            : nlohmann::json{{"found",false},{"task_id",request.task_id},
                             {"error",status.error}}.dump(2);
        conversation::ConversationEngine control_engine(
            store, [response](const auto&, const auto&) {
                conversation::ModelTurnOutcome outcome;
                outcome.reason = conversation::ModelTurnStopReason::EndTurn;
                outcome.candidate_answer = response;
                return outcome;
            }, std::move(event_sink));
        return control_engine.start_turn(request);
    }
    if(control_only) {
        conversation::TaskCommandService commands(task_registry,
            runtime.task_control_service.get(),runtime.task_command_policy.get());
        conversation::TaskCommandRequest command{
            intent == conversation::TaskInputIntent::CancelTask
                ? conversation::TaskCommandKind::Cancel
                : conversation::TaskCommandKind::Suspend,
            request, "cancelled_by_user",
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()};
        command.expected_task_revision=expected_task_revision;
        if(runtime.task_principal_resolver)
            command.principal=runtime.task_principal_resolver(identity);
        const auto controlled=commands.execute(command);
        if(!controlled.ok) throw std::runtime_error(controlled.error);
        if(runtime.task_action_observer && controlled.payload.contains("actions"))
            runtime.task_action_observer(controlled.payload["actions"],controlled.task_revision);
        const std::string response=controlled.payload.dump(2);
        conversation::ConversationEngine control_engine(
            store, [response](const auto&, const auto&) {
                conversation::ModelTurnOutcome outcome;
                outcome.reason = conversation::ModelTurnStopReason::EndTurn;
                outcome.candidate_answer = response;
                return outcome;
            }, std::move(event_sink));
        return control_engine.start_turn(request);
    }
    conversation::TaskOrchestrator task_orchestrator(task_registry);
    if(!pending_clarification && !needs_clarification) {
        const auto task_open = task_orchestrator.open_or_resume(request, intent);
        if(!task_open.ok) throw std::runtime_error(task_open.error);
        active_task = task_open.task;
    }
    if(active_task && !pending_clarification && !needs_clarification &&
       runtime.task_action_observer && runtime.task_command_policy &&
       runtime.task_principal_resolver) {
        const auto principal = runtime.task_principal_resolver(identity);
        if(principal) {
            nlohmann::json actions=nlohmann::json::array();
            for(const auto& action : runtime.task_command_policy->actions(
                    *principal, request, active_task))
                actions.push_back({{"command",conversation::name(action.command)},
                    {"required_scope",action.required_scope},{"enabled",action.enabled},
                    {"reason",action.reason}});
            runtime.task_action_observer(actions,active_task->revision);
        }
    }
    auto event_forwarder = event_sink;
    auto harness_executor = runtime.harness_turn_executor;
    std::shared_ptr<harness::SQLiteHarnessStore> interactive_harness_store;
    std::shared_ptr<conversation::HarnessTurnAdapter> interactive_harness;
    if(runtime.harness_ready && !harness_executor) {
        if(runtime.trust_profile == ExecutionTrustProfile::Production)
            throw std::runtime_error("production_harness_executor_not_injected");
        if(runtime.task_profile != conversation::TaskExecutionProfile::Conversation &&
           runtime.task_profile != conversation::TaskExecutionProfile::ReadOnlyAnalysis)
            throw std::runtime_error(
                "production_harness_executor_required_for_task_profile");
        const char* configured_harness_db = std::getenv("AGENT_HARNESS_DB");
        const auto harness_database = configured_harness_db && *configured_harness_db
            ? std::filesystem::path(configured_harness_db)
            : std::filesystem::path(".agent-framework") /
                  (std::string(agent_name) + "-harness.sqlite3");
        interactive_harness_store = std::make_shared<harness::SQLiteHarnessStore>(
            harness_database.string());
        interactive_harness = std::make_shared<conversation::HarnessTurnAdapter>(
            *interactive_harness_store,
            [callback = graph](const auto&) mutable {
                return conversation::GraphTurnAdapter::from_workflow(callback());
            }, conversation::HarnessTurnAdapter::ProjectionSink{},
            [&store, identity, turn_id = request.turn_id,
                    run_id = request.run_id, event_forwarder](const auto& source) {
                conversation::RuntimeEventEnvelope event;
                event.tenant_id = identity.tenant_id;
                event.conversation_id = identity.conversation_id;
                event.turn_id = turn_id;
                event.run_id = run_id;
                event.sequence = store.last_event_sequence(identity) + 1;
                event.event_id = source.harness_id + ":" +
                    std::to_string(source.sequence);
                event.durability = conversation::EventDurability::Durable;
                event.visibility = conversation::EventVisibility::Operations;
                event.event_type = "harness." + source.event_type;
                event.timestamp = source.created_at;
                event.payload = source.payload;
                event.payload["harness_id"] = source.harness_id;
                event.payload["harness_sequence"] = source.sequence;
                event.payload["checkpoint_revision"] = source.checkpoint_revision;
                std::string error;
                if(!store.append_event(event, &error))
                    throw std::runtime_error(
                        "conversation_harness_trace_commit_failed:" + error);
                if(event_forwarder) event_forwarder(event);
            });
        harness_executor = [interactive_harness](const auto& request) {
            return interactive_harness->execute(request);
        };
    }
    conversation::HarnessSupportedTurnRuntime supported(
        {runtime.trust_profile == ExecutionTrustProfile::Production,
         runtime.harness_ready, static_cast<bool>(runtime.long_task_executor),
         runtime.explicit_legacy_fallback},
        std::move(harness_executor),
        [callback = std::move(graph)](const auto&) mutable {
            return conversation::GraphTurnAdapter::from_workflow(callback());
        }, event_forwarder, runtime.long_task_executor);
    if(pending_clarification) {
        conversation::TaskClarificationCoordinator coordinator(
            store,clarification_store,task_registry);
        const auto resumed=coordinator.answer_pending(
            identity,request.input,
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()),
            [&supported](const conversation::TurnRequest& restored,
                         const conversation::TurnCheckpoint& checkpoint) {
                return supported.execute(restored,checkpoint);
            },std::move(event_sink));
        if(!resumed.handled||!resumed.error.empty())
            return {resumed.turn.checkpoint,resumed.turn.outcome,
                    resumed.error.empty()?"clarification_resume_not_handled":resumed.error};
        return resumed.turn;
    }
    if(needs_clarification && classification) {
        conversation::TaskClarificationCoordinator coordinator(
            store,clarification_store,task_registry);
        const auto now_ms=static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        return coordinator.begin(request,intent,*classification,now_ms,
                                 15ULL*60ULL*1000ULL,std::move(event_sink));
    }
    conversation::ConversationEngine engine(
        store,
        [&supported](const conversation::TurnRequest& request,
                     const conversation::TurnCheckpoint& checkpoint) {
            return supported.execute(request, checkpoint);
        }, std::move(event_sink));
    return engine.start_turn(request);
}

inline ExecutionTrustProfile execution_trust_profile_from_env() {
    const char* raw = std::getenv("AGENT_EXECUTION_PROFILE");
    const std::string value = raw && *raw ? raw : "demo";
    if(value == "demo") return ExecutionTrustProfile::Demo;
    if(value == "test") return ExecutionTrustProfile::Test;
    if(value == "production") return ExecutionTrustProfile::Production;
    throw std::invalid_argument(
        "AGENT_EXECUTION_PROFILE must be one of: demo, test, production");
}

inline conversation::TaskExecutionProfile task_execution_profile_from_env() {
    const char* raw = std::getenv("AGENT_TASK_PROFILE");
    const std::string value = raw && *raw ? raw : "conversation";
    auto parsed = conversation::task_execution_profile(value);
    if(!parsed) throw std::invalid_argument(
        "AGENT_TASK_PROFILE must be one of: conversation, read_only_analysis, "
        "artifact_delivery, code_change, external_action, professional");
    return *parsed;
}

inline std::optional<conversation::TaskExecutionProfile> configured_task_profile_from_env() {
    const char* raw=std::getenv("AGENT_TASK_PROFILE");
    if(!raw||!*raw)return std::nullopt;
    auto parsed=conversation::task_execution_profile(raw);
    if(!parsed)throw std::invalid_argument(
        "AGENT_TASK_PROFILE must be one of: conversation, read_only_analysis, "
        "artifact_delivery, code_change, external_action, professional");
    return parsed;
}

inline void require_harness_supported_execution(const LiveRuntime& runtime) {
    if(runtime.trust_profile == ExecutionTrustProfile::Production &&
       (!runtime.harness_ready || !runtime.harness_turn_executor ||
        !runtime.long_task_executor || !runtime.task_control_service))
        throw std::runtime_error(
            "production_workflow_executors_required: inject production Harness and "
            "LongTaskWorkflow executors");
}

inline void set_environment_override(const char* key, const std::string& value) {
#if defined(_WIN32)
    (void)_putenv_s(key, value.c_str());
#else
    (void)::setenv(key, value.c_str(), 1);
#endif
}

inline void apply_skill_cli_options(const std::string& skills_root,
                                    const std::string& authoring_root,
                                    bool disabled) {
    if(!skills_root.empty()) set_environment_override("AGENT_SKILLS_DIR", skills_root);
    if(!authoring_root.empty())
        set_environment_override("AGENT_SKILL_AUTHORING_DIR", authoring_root);
    if(disabled) set_environment_override("AGENT_SKILLS_DISABLED", "1");
}

inline SkillUiStatus skill_ui_status(const std::shared_ptr<SkillServices>& services) {
    SkillUiStatus result;
    if(!services || !services->registry) return result;
    result.enabled = true;
    const auto snapshot = services->registry->snapshot();
    result.count = snapshot.entries().size();
    result.generation = snapshot.generation();
    result.diagnostics = snapshot.diagnostics().size();
    result.errors = static_cast<std::size_t>(std::count_if(
        snapshot.diagnostics().begin(), snapshot.diagnostics().end(),
        [](const SkillDiagnostic& diagnostic) {
            return diagnostic.severity == SkillDiagnosticSeverity::Error;
        }));
    if(!services->registry->roots().empty())
        result.root = services->registry->roots().front().string();
    if(services->manager) {
        const auto status = services->manager->status();
        const auto active = status.find("activeSkill");
        if(active != status.end() && active->is_string()) result.active = active->get<std::string>();
    }
    if(result.active.empty()) result.active = "-";
    return result;
}

inline std::vector<std::string> comma_separated_env(const char* name) {
    std::vector<std::string> values;
    const char* raw = std::getenv(name);
    if(!raw || !*raw) return values;
    std::string item;
    for(const char ch : std::string(raw)) {
        if(ch == ',') {
            if(!item.empty()) values.push_back(item);
            item.clear();
        } else if(ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
            item.push_back(ch);
        }
    }
    if(!item.empty()) values.push_back(item);
    return values;
}

inline bool env_truthy(const char* name) {
    const char* value = std::getenv(name);
    if(!value) return false;
    const std::string text(value);
    return !text.empty() && text != "0" && text != "false" && text != "FALSE" && text != "off";
}

inline std::string cursor_mcp_config_path(const std::string& explicit_path) {
    if(!explicit_path.empty()) return explicit_path;
    if(const char* configured = std::getenv("AGENT_TEST_CURSOR_MCP_JSON"); configured && *configured)
        return configured;
    return {};
}

inline std::shared_ptr<SkillServices> discover_skill_services(const BootstrapOptions& options) {
    if(env_truthy("AGENT_SKILLS_DISABLED")) return nullptr;
    if(!options.skills_root.empty()) {
#if defined(_WIN32)
        _putenv_s("AGENT_SKILLS_DIR", options.skills_root.string().c_str());
#else
        ::setenv("AGENT_SKILLS_DIR", options.skills_root.string().c_str(), 1);
#endif
        return SkillServices::from_env();
    }
    if(const char* configured = std::getenv("AGENT_SKILLS_DIR"); configured && *configured)
        return SkillServices::from_env();
    return options.use_cursor_skill_roots ? SkillServices::from_default_skill_roots()
                                          : nullptr;
}

inline BootstrapResult bootstrap_agent_services(ToolBus& bus, const BootstrapOptions& options) {
    BootstrapResult result;
    result.skills = discover_skill_services(options);
    if(!result.skills) result.diagnostics.emplace_back("no skill roots were discovered");

    if(options.import_cursor_mcp) {
        const auto config = cursor_mcp_config_path(options.cursor_mcp_config);
        auto skipped = options.skip_mcp_services;
        const auto env_skipped = comma_separated_env("AGENT_MCP_SKIP_SERVICES");
        skipped.insert(skipped.end(), env_skipped.begin(), env_skipped.end());
        std::sort(skipped.begin(), skipped.end());
        skipped.erase(std::unique(skipped.begin(), skipped.end()), skipped.end());
        try {
            const auto imported = bus.register_mcp_from_cursor_config(config, true, skipped);
            result.mcp_services = imported.registered_services.size();
            result.registered_mcp_services = imported.registered_services;
            result.skipped_mcp_services = imported.skipped_services;
            for(const auto& failure : imported.failures)
                result.diagnostics.push_back(failure.service_name + ": " + failure.reason);
        } catch(const std::exception& exception) {
            result.diagnostics.push_back(std::string("MCP import failed: ") + exception.what());
        }
    }
    return result;
}

inline void set_env_if_absent(const char* key, const std::string& value) {
    if(std::getenv(key) != nullptr) return;
    set_environment_override(key, value);
}

/**
 * @brief Default AGENT_FS_ROOT to agent_framework/tools when unset (all live demos).
 *
 * Compile-time AGENT_DEMO_TOOLS_ROOT is injected by CMake for demo targets. Explicit
 * AGENT_FS_ROOT / --fs-root always wins.
 */
inline void ensure_demo_fs_root_default() {
    if(std::getenv("AGENT_FS_ROOT") != nullptr) return;
#if defined(AGENT_DEMO_TOOLS_ROOT)
    const std::filesystem::path tools_root{AGENT_DEMO_TOOLS_ROOT};
    std::error_code ec;
    if(std::filesystem::is_directory(tools_root, ec)) {
        set_environment_override("AGENT_FS_ROOT", tools_root.string());
    }
#endif
}

/** Apply the provider defaults formerly copied into every live demo. */
inline void apply_live_llm_env_defaults() {
    ensure_demo_fs_root_default();
    if(std::getenv("OPENAI_API_KEY") == nullptr) {
        if(const char* key = std::getenv("DEEPSEEK_API_KEY"); key && *key)
            set_environment_override("OPENAI_API_KEY", key);
    }
    set_env_if_absent("AGENT_OPENAI_BASE_URL", "https://api.deepseek.com/v1");
    set_env_if_absent("AGENT_LLM_PROVIDER", "openai");
    set_env_if_absent("AGENT_HTTP_TIMEOUT_SEC", "120");
    set_env_if_absent("AGENT_LLM_MAX_RETRIES", "1");
    if(std::getenv("AGENT_LLM_MODEL") == nullptr) {
        const char* configured = std::getenv("DEEPSEEK_MODEL");
        set_environment_override("AGENT_LLM_MODEL",
                                 configured && *configured ? configured : "deepseek-chat");
    }
}

inline ToolMeta live_demo_add_tool_meta() {
    ToolMeta meta;
    meta.name = "add";
    meta.description = "sum two integers";
    meta.schema = nlohmann::json::parse(R"({"type":"object","properties":{"a":{"type":"integer"},"b":{"type":"integer"}},"required":["a","b"]})");
    return meta;
}

inline void register_live_demo_tools(ToolBus& bus) {
    bus.register_local_tool("add", [](const nlohmann::json& value) {
        return nlohmann::json{{"result", value.at("a").get<int>() + value.at("b").get<int>()}};
    }, live_demo_add_tool_meta());
}

inline std::string live_system_prompt(bool mcp_available, bool skills_enabled) {
    std::string prompt = mcp_available
        ? "你是一个能够调用外部工具的助手。若有与问题直接相关的工具，优先调用工具获取可核对的信息；"
          "若无完全对口工具，可结合现有工具输出与常识推理补全结论。\n"
        : "你是一个助手。当前未加载 MCP 工具；请基于常识与公开典型情况回答，并明确标注为估算。\n";
    prompt += "不要编造无法核对的细节；信息不足时请说明假设并给出合理区间。\n";
    if(skills_enabled)
        prompt += "若系统提示中带有 Active skill，请优先遵循该技能说明；run_skill_script 的 skill_id 须为已索引技能的 canonical 名，且需配置 allowlist。\n";
    prompt += "回答使用简体中文，结构清晰。\n";
    return prompt;
}

inline LiveRuntime build_live_runtime(const LiveRuntimeOptions& options) {
    if(options.agent_name.empty()) throw std::invalid_argument("LiveRuntimeOptions.agent_name is required");
    if(!options.provider.empty()) set_environment_override("AGENT_LLM_PROVIDER", options.provider);
    if(!options.model.empty()) set_environment_override("AGENT_LLM_MODEL", options.model);
    apply_live_llm_env_defaults();

    LiveRuntime runtime;
    runtime.trust_profile = execution_trust_profile_from_env();
    runtime.task_profile = task_execution_profile_from_env();
    runtime.configured_task_profile = configured_task_profile_from_env();
    runtime.explicit_legacy_fallback = env_truthy("AGENT_LEGACY_REACT_FALLBACK");
    runtime.task_command_policy = options.task_command_policy
        ? options.task_command_policy
        : std::make_shared<conversation::TaskCommandPolicy>();
    runtime.task_principal_resolver = options.task_principal_resolver;
    runtime.task_action_observer = options.task_action_observer;
    if(!runtime.task_principal_resolver &&
       runtime.trust_profile != ExecutionTrustProfile::Production) {
        runtime.task_principal_resolver=[](const conversation::ConversationIdentity& identity)
            -> std::optional<conversation::TaskCommandPrincipal> {
            return conversation::TaskCommandPrincipal{"local-interactive-user",identity,
                {"task:read","task:write","task:control"},true};
        };
    }
    if(runtime.trust_profile == ExecutionTrustProfile::Production &&
       runtime.explicit_legacy_fallback)
        throw std::runtime_error("production_legacy_react_fallback_forbidden");
    // Interactive deployments are harness-supported by default. Production remains fail-closed
    // until a DefaultProductionCompositionBuilder-backed executor is injected by deployment.
    runtime.harness_ready = runtime.trust_profile != ExecutionTrustProfile::Production;
    runtime.llm = std::make_shared<LLMClient>(LLMClient::from_env());
    runtime.llm->set_prompt_renderer(std::make_shared<PromptRenderer>());
    const char* classifier_mode=std::getenv("AGENT_TASK_CLASSIFIER");
    if(!classifier_mode||std::string(classifier_mode)!="off") {
        const std::string provider=std::getenv("AGENT_LLM_PROVIDER")
            ?std::getenv("AGENT_LLM_PROVIDER"):std::string{};
        runtime.task_classifier=std::make_shared<conversation::LLMTaskClassifier>(
            runtime.llm,provider);
    }
    if(const char* strategy = std::getenv("AGENT_MEMORY_COMPACTOR");
       strategy && std::string(strategy) == "structured") {
        runtime.memory_compaction_llm = std::make_shared<LLMClient>(LLMClient::from_env());
        runtime.memory_compaction_llm->set_prompt_renderer(std::make_shared<PromptRenderer>());
        const std::string provider = std::getenv("AGENT_LLM_PROVIDER")
            ? std::getenv("AGENT_LLM_PROVIDER") : "openai";
        ModelConfig summary_profile;
        summary_profile.temperature = 0.0;
        summary_profile.stream = false;
        summary_profile.max_retries = 0;
        if(const char* model = std::getenv("AGENT_MEMORY_SUMMARY_MODEL"))
            summary_profile.model_name = model;
        if(const char* timeout = std::getenv("AGENT_MEMORY_SUMMARY_TIMEOUT_MS")) {
            const int parsed = std::atoi(timeout);
            if(parsed > 0) summary_profile.http_timeout_sec = std::max(1, parsed / 1000);
        }
        runtime.memory_compaction_llm->configure(provider, summary_profile);
    }
    runtime.toolbus = std::make_shared<ToolBus>();
    register_live_demo_tools(*runtime.toolbus);

    BootstrapOptions bootstrap_options;
    bootstrap_options.skills_root = options.skills_root;
    bootstrap_options.cursor_mcp_config = options.cursor_mcp_config;
    bootstrap_options.use_cursor_skill_roots = options.use_cursor_skill_roots;
    bootstrap_options.import_cursor_mcp = options.import_cursor_mcp;
    bootstrap_options.verbose = options.verbose;
    bootstrap_options.skip_mcp_services = options.skip_mcp_services;
    if(!options.enable_skills) set_environment_override("AGENT_SKILLS_DISABLED", "1");
    runtime.bootstrap = bootstrap_agent_services(*runtime.toolbus, bootstrap_options);
    runtime.skills = runtime.bootstrap.skills;
    if(runtime.skills && runtime.skills->manager)
        runtime.skills->manager->attach_toolbus(runtime.toolbus);
    if(runtime.skills) register_skill_discovery_tool(*runtime.toolbus,runtime.skills);

    runtime.toolbus->ensure_default_tools_registered([&] {
        register_builtin_fs_tools_if_configured(*runtime.toolbus);
        register_builtin_process_tools_if_configured(*runtime.toolbus);
        register_builtin_web_tools_if_configured(*runtime.toolbus);
        register_builtin_expr_tools_if_configured(*runtime.toolbus);
        register_builtin_draw_tools_if_configured(*runtime.toolbus);
    });
    runtime.skill_runners = agent_template::build_production_toolbus_runners(runtime.toolbus);
    if(options.production_resources) {
        if(runtime.trust_profile != ExecutionTrustProfile::Production)
            throw std::runtime_error(
                "production_resources_require_production_execution_profile");
        auto production = agent_framework::runtime::ProductionLiveRuntime::build(
            *options.production_resources);
        if(!production)
            throw std::runtime_error("production_runtime_build_failed:" + production.error);
        runtime.production_runtime = std::move(production.runtime);
        runtime.harness_turn_executor = runtime.production_runtime->response_executor();
        runtime.long_task_executor = runtime.production_runtime->long_task_executor();
        runtime.task_control_service = runtime.production_runtime->task_control_service();
        runtime.harness_ready = true;
    }
    runtime.composition_report={{"profile",runtime.trust_profile==ExecutionTrustProfile::Production?"production":runtime.trust_profile==ExecutionTrustProfile::Test?"test":"demo"},
        {"toolbus",true},{"skills",runtime.skills!=nullptr},{"runner_local",bool(runtime.skill_runners->resolve(agent_template::SkillRunnerKind::LocalCapability))},
        {"runner_process",bool(runtime.skill_runners->resolve(agent_template::SkillRunnerKind::SandboxedProcess))},{"runner_cli",bool(runtime.skill_runners->resolve(agent_template::SkillRunnerKind::Cli))},
        {"runner_mcp",bool(runtime.skill_runners->resolve(agent_template::SkillRunnerKind::Mcp))},{"runner_child",bool(runtime.skill_runners->resolve(agent_template::SkillRunnerKind::ChildAgent))},
        {"runner_nested",bool(runtime.skill_runners->resolve(agent_template::SkillRunnerKind::NestedWorkflow))},{"runner_approval",bool(runtime.skill_runners->resolve(agent_template::SkillRunnerKind::HumanApproval))},
        {"harness_ready",runtime.harness_ready},{"long_task_ready",bool(runtime.long_task_executor)},
        {"task_control_ready",bool(runtime.task_control_service)},
        {"legacy_fallback",runtime.explicit_legacy_fallback}};
    if(runtime.production_runtime) {
        const auto& report = runtime.production_runtime->report();
        runtime.composition_report["production_dependency_manifest_digest"] =
            report.dependency_manifest_digest;
        runtime.composition_report["production_composition_manifest_digest"] =
            report.composition_manifest_digest;
        runtime.composition_report["production_deployment_manifest_digest"] =
            report.deployment_manifest_digest;
        runtime.composition_report["production_runtime_readiness"] =
            runtime.production_runtime->readiness_manifest();
    }
    runtime.composition_report["production_ready"]=runtime.trust_profile==ExecutionTrustProfile::Production&&runtime.harness_ready&&runtime.harness_turn_executor&&runtime.long_task_executor&&runtime.task_control_service&&runtime.composition_report["runner_child"].get<bool>()&&runtime.composition_report["runner_nested"].get<bool>()&&runtime.composition_report["runner_approval"].get<bool>();
    runtime.config.name = options.agent_name;
    runtime.config.system_prompt = live_system_prompt(runtime.bootstrap.mcp_services > 0,
                                                      runtime.skills != nullptr);
    if(runtime.skills && runtime.skills->registry && env_truthy("AGENT_SKILL_INJECT_CATALOG")) {
        std::size_t cap = 2048;
        if(const char* raw = std::getenv("AGENT_SKILL_CATALOG_MAX_CHARS")) {
            const int parsed = std::atoi(raw);
            if(parsed > 0) cap = static_cast<std::size_t>(parsed);
        }
        runtime.config.system_prompt += format_skill_catalog_l1(*runtime.skills->registry, cap);
    }
    if(const char* model = std::getenv("AGENT_LLM_MODEL")) runtime.config.model_config.model_name = model;
    if(options.max_iterations > 0) runtime.config.max_iterations = options.max_iterations;
    runtime.input_policy.tier_b_enabled = options.enable_tier_b;
    runtime.input_policy.tier_b_llm = options.enable_tier_b ? runtime.llm : nullptr;
    return runtime;
}

inline AgentExecutionProfile to_execution_profile(const LiveRuntime& runtime) {
    AgentExecutionProfile profile;
    profile.config = runtime.config;
    profile.deps = {runtime.llm, runtime.toolbus, runtime.skills,
                    runtime.memory_compaction_llm};
    profile.input_policy = runtime.input_policy;
    profile.trust_profile = runtime.trust_profile;
    return profile;
}

inline nlohmann::json skill_event_json(const SkillEvent& event) {
    return {{"type", skill_event_type_cstr(event.type)}, {"code", event.code},
            {"skillId", event.skill_id}, {"resourceId", event.resource_id},
            {"taskId", event.task_id}, {"runId", event.run_id},
            {"attempt", event.attempt}, {"iteration", event.iteration},
            {"details", event.details}};
}

} // namespace agent_framework::example
