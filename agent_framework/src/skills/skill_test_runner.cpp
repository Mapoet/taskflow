#include <agent/skill_test_runner.hpp>

#include <algorithm>
#include <fstream>
#include <set>

namespace agent_framework {

namespace {

SkillTestParseResult invalid(const std::filesystem::path& source, std::string location,
                             std::string message) {
    SkillDiagnostic diagnostic;
    diagnostic.severity = SkillDiagnosticSeverity::Error;
    diagnostic.code = "skill_test_descriptor_invalid";
    diagnostic.path = source;
    diagnostic.location = std::move(location);
    diagnostic.message = std::move(message);
    diagnostic.suggestion = "conform to agent.taskflow/skill-test/v1";
    return {std::nullopt, {std::move(diagnostic)}};
}

bool exact_keys(const nlohmann::json& object, const std::set<std::string>& allowed) {
    if(!object.is_object()) return false;
    for(auto it = object.begin(); it != object.end(); ++it)
        if(!allowed.contains(it.key())) return false;
    return true;
}

bool safe_resource_id(const std::string& value) {
    return !value.empty() && value.front() != '/' && value.find("..") == std::string::npos &&
           value.find('\\') == std::string::npos;
}

} // namespace

SkillTestParseResult parse_skill_test_descriptor(const nlohmann::json& value,
                                                  const std::filesystem::path& source) {
    if(!value.is_object()) return invalid(source, "", "descriptor must be an object");
    if(value.dump().size() > 1024 * 1024)
        return invalid(source, "/input", "descriptor exceeds the one MiB limit");
    const std::set<std::string> top = {
        "apiVersion", "kind", "name", "target", "input", "mocks", "expect"};
    if(!exact_keys(value, top)) return invalid(source, "", "descriptor contains unknown fields");
    if(value.value("apiVersion", "") != "agent.taskflow/skill-test/v1")
        return invalid(source, "/apiVersion", "unsupported test descriptor version");
    if(value.value("kind", "") != "SkillTest")
        return invalid(source, "/kind", "kind must be SkillTest");
    if(!value.contains("name") || !value["name"].is_string() || value["name"].get<std::string>().empty())
        return invalid(source, "/name", "name must be a non-empty string");
    if(!value.contains("target") ||
       !exact_keys(value["target"], {"kind", "resource"}))
        return invalid(source, "/target", "target must contain only kind and resource");
    const auto target_kind = value["target"].value("kind", "");
    const std::set<std::string> target_kinds = {"resource", "tool", "workflow", "script", "cli"};
    if(!target_kinds.contains(target_kind))
        return invalid(source, "/target/kind", "unsupported target kind");
    const auto resource = value["target"].value("resource", "");
    if(!safe_resource_id(resource))
        return invalid(source, "/target/resource", "target resource contains an unsafe path");
    if(value.contains("mocks")) {
        if(!exact_keys(value["mocks"], {"tools"}))
            return invalid(source, "/mocks", "only deterministic tool mocks are allowed");
        if(value["mocks"].contains("tools") && !value["mocks"]["tools"].is_object())
            return invalid(source, "/mocks/tools", "tool mocks must be an object");
        if(value["mocks"].contains("tools")) {
            for(auto it = value["mocks"]["tools"].begin(); it != value["mocks"]["tools"].end(); ++it) {
                if(!exact_keys(it.value(), {"output", "error"}))
                    return invalid(source, "/mocks/tools/" + it.key(), "mock contains unknown fields");
            }
        }
    }
    if(!value.contains("expect") ||
       !exact_keys(value["expect"], {"ok", "output", "error", "events", "stdout",
                                     "stderr", "exitCode", "resourceDigests"}) ||
       !value["expect"].contains("ok") || !value["expect"]["ok"].is_boolean())
        return invalid(source, "/expect", "expect.ok is required and unknown fields are forbidden");
    if(value["expect"].contains("events")) {
        if(!value["expect"]["events"].is_array())
            return invalid(source, "/expect/events", "events must be an array");
        for(std::size_t index = 0; index < value["expect"]["events"].size(); ++index) {
            const auto& event = value["expect"]["events"][index];
            if(!exact_keys(event, {"type"}) || !event.contains("type") || !event["type"].is_string())
                return invalid(source, "/expect/events/" + std::to_string(index),
                               "event expectations require only a string type");
        }
    }
    SkillTestDescriptor descriptor;
    descriptor.name = value["name"].get<std::string>();
    descriptor.target = {target_kind, resource};
    descriptor.input = value.value("input", nlohmann::json::object());
    descriptor.mocks = value.value("mocks", nlohmann::json::object());
    descriptor.expect = value["expect"];
    descriptor.source = source;
    return {std::move(descriptor), {}};
}

SkillTestParseResult parse_skill_test_file(const std::filesystem::path& path) {
    try {
        std::ifstream input(path);
        if(!input) return invalid(path, "", "test descriptor could not be opened");
        nlohmann::json value;
        input >> value;
        return parse_skill_test_descriptor(value, path);
    } catch(const std::exception& error) {
        return invalid(path, "", std::string("invalid JSON: ") + error.what());
    }
}

} // namespace agent_framework
