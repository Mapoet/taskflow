#pragma once

#include <agent/skill_runtime.hpp>
#include <agent/skill_services.hpp>
#include <agent/toolbus.hpp>

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
};

struct BootstrapResult {
    std::shared_ptr<SkillServices> skills;
    std::size_t mcp_services = 0;
    std::vector<std::string> diagnostics;
};

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
        try {
            const auto imported = bus.register_mcp_from_cursor_config(config, true);
            result.mcp_services = imported.registered_services.size();
            for(const auto& failure : imported.failures)
                result.diagnostics.push_back(failure.service_name + ": " + failure.reason);
        } catch(const std::exception& exception) {
            result.diagnostics.push_back(std::string("MCP import failed: ") + exception.what());
        }
    }
    return result;
}

inline nlohmann::json skill_event_json(const SkillEvent& event) {
    return {{"type", skill_event_type_cstr(event.type)}, {"code", event.code},
            {"skillId", event.skill_id}, {"resourceId", event.resource_id},
            {"taskId", event.task_id}, {"runId", event.run_id},
            {"attempt", event.attempt}, {"iteration", event.iteration},
            {"details", event.details}};
}

} // namespace agent_framework::example
