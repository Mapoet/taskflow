#include <agent/skill_command.hpp>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace agent_framework;
namespace {

int emit(const SkillCommandResponse& response) {
    if(response.raw_output) std::cout.write(
        response.raw_output->data(), static_cast<std::streamsize>(response.raw_output->size()));
    else std::cout << response.to_json().dump(2) << '\n';
    return static_cast<int>(response.exit);
}

SkillCommandResponse usage_error(const std::string& command, const std::string& message) {
    SkillCommandResponse response;
    response.exit = SkillCliExit::Usage;
    response.command = command;
    response.error = {{"code", "skillctl_usage_error"}, {"message", message},
                      {"details", {{"usage", skillctl_usage()}}}};
    return response;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> tokens;
    for(int i = 1; i < argc; ++i) tokens.emplace_back(argv[i]);
    const auto parsed = parse_skill_cli_arguments(tokens);
    if(!parsed) {
        std::cout << parsed.response.to_json().dump(2) << '\n';
        return static_cast<int>(parsed.response.exit);
    }
    if(parsed.arguments->help) {
        std::cout << skillctl_usage() << '\n';
        return 0;
    }

    const auto& invocation = *parsed.arguments;
    auto registry = std::make_shared<SkillRegistry>(invocation.root);
    registry->scan_or_reload();
    SkillCommandService service(registry);
    const auto& command = invocation.command;
    const auto& operands = invocation.operands;
    if(command == "list" && operands.empty()) return emit(service.list());
    if(command == "validate" && operands.empty()) return emit(service.validate());
    if(command == "show" && operands.size() == 1) return emit(service.show(operands[0]));
    if(command == "inspect" && !operands.empty() && operands.size() <= 2) {
        const bool resolved = operands.size() == 2 && operands[1] == "--resolved";
        if(operands.size() == 2 && !resolved)
            return emit(usage_error(command, "inspect accepts only --resolved"));
        return emit(service.inspect(operands[0], resolved));
    }
    if(command == "read" && operands.size() >= 3) {
        const auto kind = parse_skill_resource_kind(operands[1]);
        if(!kind) return emit(usage_error(command, "unknown resource kind: " + operands[1]));
        std::size_t max_bytes = 65536;
        bool raw = false;
        for(std::size_t index = 3; index < operands.size(); ++index) {
            if(operands[index] == "--raw") raw = true;
            else if(operands[index] == "--max-bytes" && index + 1 < operands.size())
                max_bytes = std::strtoull(operands[++index].c_str(), nullptr, 10);
            else if(index == 3 && !operands[index].starts_with("-"))
                max_bytes = std::strtoull(operands[index].c_str(), nullptr, 10);
            else return emit(usage_error(command, "invalid read option: " + operands[index]));
        }
        return emit(service.read(operands[0], *kind, operands[2], max_bytes, raw));
    }
    if(command == "lint") {
        std::string skill_id;
        bool warnings_as_errors = false;
        for(const auto& operand : operands) {
            if(operand == "--warnings-as-errors") warnings_as_errors = true;
            else if(skill_id.empty() && !operand.starts_with("-")) skill_id = operand;
            else return emit(usage_error(command, "invalid lint option: " + operand));
        }
        return emit(service.lint(skill_id, warnings_as_errors));
    }
    if(command == "graph" && operands.empty()) return emit(service.graph());
    if(command == "permissions" && !operands.empty()) {
        SkillPermissionGrant grant;
        for(std::size_t index = 1; index < operands.size(); index += 2) {
            if(index + 1 >= operands.size())
                return emit(usage_error(command, "permission grant value is missing"));
            const auto& option = operands[index];
            const auto& value = operands[index + 1];
            if(option == "--grant-tool") grant.tools.push_back(value);
            else if(option == "--grant-network") grant.network.push_back(value);
            else if(option == "--grant-env") grant.environment.push_back(value);
            else if(option == "--grant-read") grant.filesystem_read.push_back(value);
            else if(option == "--grant-write") grant.filesystem_write.push_back(value);
            else if(option == "--grant-secret") grant.secrets.push_back(value);
            else return emit(usage_error(command, "unknown permission option: " + option));
        }
        return emit(service.permissions(operands[0], grant));
    }
    if(command == "doctor" && !operands.empty()) {
        SkillDoctorOptions options;
        for(std::size_t index = 1; index < operands.size(); index += 2) {
            if(index + 1 >= operands.size())
                return emit(usage_error(command, "doctor option value is missing"));
            const auto& option = operands[index];
            const auto& value = operands[index + 1];
            if(option == "--runtime") options.available_runtimes.push_back(value);
            else if(option == "--model") options.available_models.push_back(value);
            else if(option == "--grant-read") options.grants.filesystem_read.push_back(value);
            else if(option == "--grant-write") options.grants.filesystem_write.push_back(value);
            else return emit(usage_error(command, "unknown doctor option: " + option));
        }
        return emit(service.doctor(operands[0], options));
    }
    if(command == "test") {
        std::string skill_id;
        std::string filter;
        std::size_t jobs = 1;
        for(std::size_t index = 0; index < operands.size(); ++index) {
            if(operands[index] == "--filter" && index + 1 < operands.size())
                filter = operands[++index];
            else if(operands[index] == "--jobs" && index + 1 < operands.size())
                jobs = std::strtoull(operands[++index].c_str(), nullptr, 10);
            else if(skill_id.empty() && !operands[index].starts_with("-"))
                skill_id = operands[index];
            else return emit(usage_error(command, "invalid test option: " + operands[index]));
        }
        return emit(service.test(skill_id, filter, jobs));
    }

    return emit(usage_error(command, "invalid command arguments"));
}
