#pragma once

#include <agent/agent_server/agent_server.hpp>
#include <agent/llm_client/llm_client.hpp>
#include <agent/prompt_renderer/prompt_renderer.hpp>
#include <agent/skills/skill_runtime.hpp>
#include <agent/skills/skill_services.hpp>
#include <agent/toolbus/draw_tools.hpp>
#include <agent/toolbus/expr_tools.hpp>
#include <agent/toolbus/fs_tools.hpp>
#include <agent/toolbus/toolbus.hpp>
#include <agent/toolbus/web_tools.hpp>
#include <agent/conversation/production_bridge.hpp>

#include <algorithm>
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
};

struct LiveRuntime {
    std::shared_ptr<LLMClient> llm;
    std::shared_ptr<LLMClient> memory_compaction_llm;
    std::shared_ptr<ToolBus> toolbus;
    std::shared_ptr<SkillServices> skills;
    AgentConfig config;
    InputPolicyConfig input_policy;
    ExecutionTrustProfile trust_profile{ExecutionTrustProfile::Demo};
    conversation::TaskExecutionProfile task_profile{
        conversation::TaskExecutionProfile::Conversation};
    BootstrapResult bootstrap;
};

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

inline void require_direct_demo_execution(const LiveRuntime& runtime) {
    if(runtime.trust_profile == ExecutionTrustProfile::Production)
        throw std::runtime_error(
            "production_direct_react_execution_forbidden: use the production harness "
            "and TaskClosureController binding");
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
    return options.use_cursor_skill_roots ? SkillServices::from_cursor_default_skill_roots()
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

/** Apply the provider defaults formerly copied into every live demo. */
inline void apply_live_llm_env_defaults() {
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
    runtime.llm = std::make_shared<LLMClient>(LLMClient::from_env());
    runtime.llm->set_prompt_renderer(std::make_shared<PromptRenderer>());
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

    runtime.toolbus->ensure_default_tools_registered([&] {
        register_builtin_fs_tools_if_configured(*runtime.toolbus);
        register_builtin_web_tools_if_configured(*runtime.toolbus);
        register_builtin_expr_tools_if_configured(*runtime.toolbus);
        register_builtin_draw_tools_if_configured(*runtime.toolbus);
    });
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
