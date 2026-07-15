#include <agent/skill_command.hpp>

#include <algorithm>
#include <array>

namespace agent_framework {

namespace {

nlohmann::json diagnostic_to_json(const SkillDiagnostic& diagnostic) {
    return {
        {"severity", diagnostic.severity == SkillDiagnosticSeverity::Error ? "error" : "warning"},
        {"code", diagnostic.code},
        {"path", diagnostic.path.generic_string()},
        {"location", diagnostic.location},
        {"message", diagnostic.message},
        {"suggestion", diagnostic.suggestion},
    };
}

SkillCliParseResult usage_error(std::string message) {
    SkillCliParseResult result;
    result.response.exit = SkillCliExit::Usage;
    result.response.command = "parse";
    result.response.error = {
        {"code", "skillctl_usage_error"},
        {"message", std::move(message)},
        {"details", {{"usage", skillctl_usage()}}},
    };
    return result;
}

nlohmann::json entry_to_json(const SkillIndexEntry& entry) {
    return {{"id", entry.id}, {"name", entry.name}, {"description", entry.description},
            {"version", entry.version}, {"license", entry.license}, {"tags", entry.tags},
            {"triggerKeywords", entry.trigger_keywords}, {"scripts", entry.scripts},
            {"references", entry.references}, {"cli", entry.cli_programs},
            {"allowedTools", entry.allowed_tools},
            {"disableModelInvocation", entry.disable_model_invocation}};
}

SkillCommandResponse command_error(SkillCliExit exit, std::string command,
                                   std::string code, std::string message,
                                   nlohmann::json details = nlohmann::json::object()) {
    SkillCommandResponse response;
    response.exit = exit;
    response.command = std::move(command);
    response.error = {{"code", std::move(code)}, {"message", std::move(message)},
                      {"details", std::move(details)}};
    return response;
}

SkillResourceType resource_type(SkillResourceKind kind) {
    switch(kind) {
        case SkillResourceKind::Script: return SkillResourceType::Script;
        case SkillResourceKind::Cli: return SkillResourceType::Cli;
        case SkillResourceKind::Reference: return SkillResourceType::Reference;
        case SkillResourceKind::Tool: return SkillResourceType::Tool;
        case SkillResourceKind::Mcp: return SkillResourceType::Mcp;
        case SkillResourceKind::Template: return SkillResourceType::Template;
        case SkillResourceKind::Schema: return SkillResourceType::Schema;
        case SkillResourceKind::Prompt: return SkillResourceType::Prompt;
        case SkillResourceKind::Workflow: return SkillResourceType::Workflow;
        case SkillResourceKind::Config: return SkillResourceType::Config;
        case SkillResourceKind::Asset: return SkillResourceType::Asset;
        case SkillResourceKind::Model: return SkillResourceType::Model;
        case SkillResourceKind::Test: return SkillResourceType::Test;
        case SkillResourceKind::AnyDeclared: return SkillResourceType::Unknown;
    }
    return SkillResourceType::Unknown;
}

bool valid_utf8_text(const std::string& value) {
    std::size_t index = 0;
    while(index < value.size()) {
        const auto byte = static_cast<unsigned char>(value[index]);
        if(byte == 0) return false;
        if(byte < 0x80) {
            if(byte < 0x09 || (byte > 0x0D && byte < 0x20)) return false;
            ++index;
            continue;
        }
        std::size_t continuation = 0;
        if((byte & 0xE0) == 0xC0) continuation = 1;
        else if((byte & 0xF0) == 0xE0) continuation = 2;
        else if((byte & 0xF8) == 0xF0) continuation = 3;
        else return false;
        if(index + continuation >= value.size()) return false;
        for(std::size_t offset = 1; offset <= continuation; ++offset) {
            if((static_cast<unsigned char>(value[index + offset]) & 0xC0) != 0x80) return false;
        }
        index += continuation + 1;
    }
    return true;
}

std::string base64_encode(const std::string& input) {
    static constexpr std::array<char, 64> alphabet = {
        'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P',
        'Q','R','S','T','U','V','W','X','Y','Z','a','b','c','d','e','f',
        'g','h','i','j','k','l','m','n','o','p','q','r','s','t','u','v',
        'w','x','y','z','0','1','2','3','4','5','6','7','8','9','+','/'
    };
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);
    for(std::size_t index = 0; index < input.size(); index += 3) {
        const auto a = static_cast<unsigned char>(input[index]);
        const auto b = index + 1 < input.size() ? static_cast<unsigned char>(input[index + 1]) : 0;
        const auto c = index + 2 < input.size() ? static_cast<unsigned char>(input[index + 2]) : 0;
        const auto value = (static_cast<unsigned>(a) << 16) |
                           (static_cast<unsigned>(b) << 8) | static_cast<unsigned>(c);
        output.push_back(alphabet[(value >> 18) & 0x3F]);
        output.push_back(alphabet[(value >> 12) & 0x3F]);
        output.push_back(index + 1 < input.size() ? alphabet[(value >> 6) & 0x3F] : '=');
        output.push_back(index + 2 < input.size() ? alphabet[value & 0x3F] : '=');
    }
    return output;
}

std::string inferred_media_type(const std::string& path, bool text) {
    const auto extension = std::filesystem::path(path).extension().string();
    if(extension == ".md") return "text/markdown";
    if(extension == ".json") return "application/json";
    if(extension == ".yaml" || extension == ".yml") return "application/yaml";
    if(extension == ".txt" || extension == ".sh" || text) return "text/plain";
    return "application/octet-stream";
}

} // namespace

bool SkillCommandResponse::ok() const noexcept {
    return exit == SkillCliExit::Success;
}

nlohmann::json SkillCommandResponse::to_json() const {
    auto serialized_diagnostics = nlohmann::json::array();
    for(const auto& diagnostic : diagnostics) {
        serialized_diagnostics.push_back(diagnostic_to_json(diagnostic));
    }

    return {
        {"apiVersion", "agent.taskflow/skillctl-output/v1"},
        {"command", command},
        {"ok", ok()},
        {"data", data},
        {"diagnostics", std::move(serialized_diagnostics)},
        {"error", error},
    };
}

SkillCliParseResult parse_skill_cli_arguments(const std::vector<std::string>& tokens) {
    if(tokens.size() == 1 && (tokens.front() == "--help" || tokens.front() == "-h")) {
        SkillCliParseResult result;
        result.arguments = SkillCliArguments{};
        result.arguments->help = true;
        return result;
    }

    SkillCliArguments parsed;
    std::size_t index = 0;
    while(index < tokens.size() && tokens[index].starts_with("-")) {
        const auto& option = tokens[index++];
        if(option == "--help" || option == "-h") {
            parsed.help = true;
            continue;
        }
        if(option != "--root" && option != "--store" && option != "--format") {
            return usage_error("unknown option: " + option);
        }
        if(index >= tokens.size() || tokens[index].starts_with("-")) {
            return usage_error("missing value for " + option);
        }
        const auto value = tokens[index++];
        if(option == "--root") parsed.root = value;
        else if(option == "--store") parsed.store = value;
        else parsed.format = value;
    }

    if(parsed.root.empty() && index < tokens.size()) {
        parsed.root = tokens[index++];
    }
    if(parsed.format != "json") {
        return usage_error("unsupported format: " + parsed.format);
    }
    if(parsed.help) {
        SkillCliParseResult result;
        result.arguments = std::move(parsed);
        return result;
    }
    if(parsed.root.empty()) return usage_error("a skills root is required");
    if(index >= tokens.size()) return usage_error("a command is required");

    parsed.command = tokens[index++];
    parsed.operands.assign(tokens.begin() + static_cast<std::ptrdiff_t>(index), tokens.end());
    SkillCliParseResult result;
    result.arguments = std::move(parsed);
    return result;
}

std::string skillctl_usage() {
    return "usage: skillctl [--root ROOT] [--store STORE] [--format json] COMMAND [ARGS...]\n"
           "       skillctl ROOT COMMAND [ARGS...]";
}

SkillCommandService::SkillCommandService(std::shared_ptr<SkillRegistry> registry)
    : registry_(std::move(registry)), loader_(*registry_) {}

SkillCommandResponse SkillCommandService::list() const {
    auto entries = registry_->entries();
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    auto skills = nlohmann::json::array();
    for(const auto& entry : entries) skills.push_back(entry_to_json(entry));
    SkillCommandResponse response;
    response.command = "list";
    response.data = {{"skills", std::move(skills)},
                     {"generation", registry_->snapshot().generation()}};
    return response;
}

SkillCommandResponse SkillCommandService::validate() const {
    SkillCommandResponse response;
    response.command = "validate";
    response.diagnostics = registry_->diagnostics();
    response.data = {{"valid", registry_->valid()}, {"skills", registry_->entries().size()},
                     {"generation", registry_->snapshot().generation()}};
    if(!registry_->valid()) {
        response.exit = SkillCliExit::ContractFailed;
        response.error = {{"code", "skill_contract_failed"},
                          {"message", "the skill registry contains contract errors"},
                          {"details", nlohmann::json::object()}};
    }
    return response;
}

SkillCommandResponse SkillCommandService::show(const std::string& skill_id) const {
    const auto entry = registry_->get(skill_id);
    if(!entry) return command_error(SkillCliExit::NotFound, "show", "skill_not_found",
                                    "skill not found", {{"skill", skill_id}});
    SkillCommandResponse response;
    response.command = "show";
    response.data = {{"skill", entry_to_json(*entry)}};
    return response;
}

SkillCommandResponse SkillCommandService::inspect(const std::string& skill_id,
                                                   bool resolved) const {
    const auto manifest = registry_->get_manifest(skill_id);
    if(!manifest) return command_error(SkillCliExit::NotFound, "inspect", "skill_not_found",
                                       "skill not found", {{"skill", skill_id}});
    SkillCommandResponse response;
    response.command = "inspect";
    response.data = {{"manifest", skill_manifest_to_json(*manifest, resolved)}};
    return response;
}

SkillCommandResponse SkillCommandService::read(const std::string& skill_id,
                                                SkillResourceKind kind,
                                                const std::string& relative_path,
                                                std::size_t max_bytes, bool raw) const {
    if(!registry_->get(skill_id)) {
        return command_error(SkillCliExit::NotFound, "read", "skill_not_found",
                             "skill not found", {{"skill", skill_id}});
    }
    std::string loader_error;
    const auto content = loader_.load_resource(
        skill_id, relative_path, kind, max_bytes, &loader_error);
    if(!content) return command_error(SkillCliExit::OperationFailed, "read",
                                      "skill_resource_read_failed", loader_error,
                                      {{"skill", skill_id}, {"path", relative_path}});

    const bool text = valid_utf8_text(*content);
    std::string media_type;
    if(const auto manifest = registry_->get_manifest(skill_id)) {
        const auto requested = resource_type(kind);
        for(const auto& resource : manifest->resources) {
            if(resource.path == relative_path &&
               (kind == SkillResourceKind::AnyDeclared || resource.kind == requested)) {
                media_type = resource.media_type;
                break;
            }
        }
    }
    if(media_type.empty()) media_type = inferred_media_type(relative_path, text);

    SkillCommandResponse response;
    response.command = "read";
    response.data = {{"mediaType", media_type}, {"bytes", content->size()},
                     {"encoding", text ? "utf-8" : "base64"},
                     {"content", text ? *content : base64_encode(*content)}};
    if(raw) response.raw_output = *content;
    return response;
}

std::optional<SkillResourceKind> parse_skill_resource_kind(const std::string& value) {
    static const std::array<std::pair<const char*, SkillResourceKind>, 13> kinds = {{
        {"script", SkillResourceKind::Script}, {"cli", SkillResourceKind::Cli},
        {"reference", SkillResourceKind::Reference}, {"tool", SkillResourceKind::Tool},
        {"mcp", SkillResourceKind::Mcp}, {"template", SkillResourceKind::Template},
        {"schema", SkillResourceKind::Schema}, {"prompt", SkillResourceKind::Prompt},
        {"workflow", SkillResourceKind::Workflow}, {"config", SkillResourceKind::Config},
        {"asset", SkillResourceKind::Asset}, {"model", SkillResourceKind::Model},
        {"test", SkillResourceKind::Test}
    }};
    const auto found = std::find_if(kinds.begin(), kinds.end(), [&](const auto& item) {
        return value == item.first;
    });
    if(found == kinds.end()) return std::nullopt;
    return found->second;
}

} // namespace agent_framework
