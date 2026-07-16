/**
 * @file skill_platform_demo.cpp
 * @brief Offline Stage 1-9 Skill platform reference application.
 */

#include <agent/skill_audit.hpp>
#include <agent/skill_command.hpp>
#include <agent/skill_config.hpp>
#include <agent/skill_lifecycle.hpp>
#include <agent/skill_runtime.hpp>
#include <agent/skill_sbom.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace agent_framework;
namespace fs = std::filesystem;

namespace {

nlohmann::json response_json(const SkillCommandResponse& response) {
    return response.to_json();
}

SkillPermissionGrant grant_all(const SkillPermissionSet& permissions) {
    return {permissions.tools, permissions.network, permissions.environment,
            permissions.filesystem_read, permissions.filesystem_write, permissions.secrets};
}

} // namespace

int main(int argc, char** argv) {
    if(argc < 2 || argc > 5) {
        std::cerr << "usage: skill_platform_demo <skills-root> [skill-id] [--jobs N]\n";
        return 64;
    }
    const fs::path root = argv[1];
    std::string selected;
    std::size_t jobs = 2;
    for(int index = 2; index < argc; ++index) {
        const std::string token = argv[index];
        if(token == "--jobs" && index + 1 < argc) jobs = std::strtoull(argv[++index], nullptr, 10);
        else if(selected.empty()) selected = token;
        else {
            std::cerr << "unexpected argument: " << token << '\n';
            return 64;
        }
    }

    auto registry = std::make_shared<SkillRegistry>(root);
    registry->scan_or_reload();
    const auto entries = registry->entries();
    if(selected.empty() && !entries.empty()) selected = entries.front().id;

    SkillCommandService commands(registry, root / ".skill-platform-demo-cache");
    nlohmann::json output = {
        {"apiVersion", "agent.taskflow/skill-platform-demo/v1"},
        {"root", fs::weakly_canonical(root).generic_string()},
        {"skillId", selected},
        {"management", {
            {"list", response_json(commands.list())},
            {"validate", response_json(commands.validate())},
            {"graph", response_json(commands.graph())},
            {"cacheStatus", response_json(commands.cache_status())},
            {"cacheVerify", response_json(commands.cache_verify())}
        }},
        {"runtime", nlohmann::json::object()},
        {"supplyChain", nlohmann::json::object()}
    };

    if(selected.empty() || !registry->get(selected) || !registry->get_manifest(selected)) {
        output["ok"] = false;
        output["error"] = {{"code", "skill_not_found"}, {"message", "no selectable skill"}};
        std::cout << output.dump(2) << '\n';
        return 3;
    }

    const auto entry = *registry->get(selected);
    const auto manifest = registry->get_manifest(selected);
    const auto grants = grant_all(manifest->permissions);
    output["management"]["inspect"] = response_json(commands.inspect(selected, true));
    output["management"]["permissions"] = response_json(commands.permissions(selected, grants));
    SkillDoctorOptions doctor_options;
    doctor_options.grants = grants;
    output["management"]["doctor"] = response_json(commands.doctor(selected, doctor_options));
    output["management"]["tests"] = response_json(commands.test(selected, "parallel pass", jobs));

    std::vector<nlohmann::json> events;
    std::vector<nlohmann::json> audits;
    SkillInvocationContext context;
    context.grants = grants;
    context.task_id = "skill-platform-demo-task";
    context.session_id = "skill-platform-demo-session";
    context.trace_id = "skill-platform-demo-trace";
    context.run_id = "skill-platform-demo-run";
    context.event_sink = [&](const SkillEvent& event) {
        events.push_back({{"type", skill_event_type_cstr(event.type)}, {"code", event.code},
                          {"skillId", event.skill_id}, {"resourceId", event.resource_id},
                          {"taskId", event.task_id}, {"runId", event.run_id},
                          {"attempt", event.attempt}, {"iteration", event.iteration},
                          {"details", event.details}});
    };
    context.audit_sink = [&](const SkillAuditRecord& record) { audits.push_back(record.to_json()); };

    const auto runnable = std::find_if(manifest->resources.begin(), manifest->resources.end(),
        [](const auto& resource) { return resource.kind != SkillResourceType::Test; });
    if(runnable != manifest->resources.end()) {
        auto loader = std::make_shared<SkillLoader>(*registry);
        SkillRuntime runtime(registry, loader);
        const auto begun = runtime.begin(selected, runnable->id, runnable->kind,
                                         nlohmann::json::object(), context);
        output["runtime"]["beginOk"] = begun.ok;
        if(begun.ticket) {
            const auto finished = runtime.finish(*begun.ticket, nlohmann::json::object());
            output["runtime"]["finishOk"] = finished.ok;
        } else {
            output["runtime"]["error"] = begun.error;
        }
    }
    output["runtime"]["events"] = events;
    output["runtime"]["audit"] = audits;

    for(const auto& resource : manifest->resources) {
        if(resource.kind != SkillResourceType::Config) continue;
        SkillConfigService configs;
        SkillConfigResolveOptions options;
        auto resolved = configs.resolve(entry, manifest, resource.id, options, context);
        output["runtime"]["config"] = resolved.ok ? resolved.value : resolved.error;
        break;
    }

    const auto inspected = inspect_skill_package(entry.file_path.parent_path());
    if(inspected.ok && inspected.package) {
        const auto sbom = generate_skill_sbom(*inspected.package);
        output["supplyChain"] = {
            {"bomFormat", sbom.value("bomFormat", "")},
            {"specVersion", sbom.value("specVersion", "")},
            {"components", sbom.value("components", nlohmann::json::array()).size()},
            {"packageDigest", inspected.package->package_digest}
        };
    }

    output["ok"] = registry->valid() && commands.validate().ok();
    std::cout << output.dump(2) << '\n';
    return output["ok"].get<bool>() ? 0 : 2;
}
