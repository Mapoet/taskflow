#include <agent/skill_manifest.hpp>

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace agent_framework {
namespace {

using json = nlohmann::json;

struct Line { std::size_t number; int indent; std::string text; };

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

json scalar(std::string value) {
    value = trim(std::move(value));
    if (value.empty()) return nullptr;
    if ((value.front() == '"' && value.back() == '"') ||
        (value.front() == '\'' && value.back() == '\'')) {
        return value.substr(1, value.size() - 2);
    }
    if (value == "true") return true;
    if (value == "false") return false;
    if (value == "null" || value == "~") return nullptr;
    if (std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
        try { return static_cast<std::uint64_t>(std::stoull(value)); } catch (...) {}
    }
    if (value.front() == '[' || value.front() == '{') {
        try { return json::parse(value); } catch (...) {}
        if (value.front() == '[' && value.back() == ']') {
            json array = json::array();
            std::istringstream items(value.substr(1, value.size() - 2));
            std::string item;
            while (std::getline(items, item, ',')) array.push_back(scalar(trim(item)));
            return array;
        }
    }
    return value;
}

std::vector<Line> lex(const std::string& yaml, std::vector<SkillManifestIssue>& issues) {
    std::vector<Line> out;
    std::istringstream in(yaml);
    std::string raw;
    std::size_t number = 0;
    while (std::getline(in, raw)) {
        ++number;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        if (raw.find('\t') != std::string::npos) {
            issues.push_back({true, "yaml_tab_indent", "/line/" + std::to_string(number),
                              "tabs are not allowed in manifest indentation", "use spaces"});
            continue;
        }
        int indent = 0;
        while (indent < static_cast<int>(raw.size()) && raw[indent] == ' ') ++indent;
        std::string text = raw.substr(static_cast<std::size_t>(indent));
        if (text.empty() || text[0] == '#') continue;
        if (text == "---" || text == "...") {
            issues.push_back({true, "yaml_multi_document", "/line/" + std::to_string(number),
                              "multiple YAML documents are not supported", "use one frontmatter document"});
            continue;
        }
        const auto colon = text.find(':');
        const std::string rhs = colon == std::string::npos ? text : trim(text.substr(colon + 1));
        if (text.find("<<:") != std::string::npos || rhs.rfind('&', 0) == 0 ||
            rhs.rfind('*', 0) == 0 || rhs.rfind('!', 0) == 0) {
            issues.push_back({true, "yaml_advanced_feature", "/line/" + std::to_string(number),
                              "anchors, aliases, merge keys, and tags are not supported", "expand the value explicitly"});
            continue;
        }
        out.push_back({number, indent, std::move(text)});
    }
    return out;
}

json parse_block(const std::vector<Line>& lines, std::size_t& i, int indent,
                 std::vector<SkillManifestIssue>& issues);

void parse_map_entry(json& object, const std::vector<Line>& lines, std::size_t& i, int indent,
                     const std::string& text, std::vector<SkillManifestIssue>& issues) {
    const auto colon = text.find(':');
    if (colon == std::string::npos) {
        issues.push_back({true, "yaml_expected_mapping", "/line/" + std::to_string(lines[i].number),
                          "expected key: value", "add a mapping key"});
        ++i;
        return;
    }
    const std::string key = trim(text.substr(0, colon));
    std::string rest = trim(text.substr(colon + 1));
    if (key.empty()) {
        ++i;
        return;
    }
    if (rest == "|" || rest == ">" || rest == ">-") {
        const bool literal = rest == "|";
        ++i;
        std::string value;
        while (i < lines.size() && lines[i].indent > indent) {
            if (!value.empty()) value += literal ? "\n" : " ";
            value += trim(lines[i].text);
            ++i;
        }
        object[key] = value;
        return;
    }
    ++i;
    if (!rest.empty()) object[key] = scalar(rest);
    else if (i < lines.size() && lines[i].indent > indent)
        object[key] = parse_block(lines, i, lines[i].indent, issues);
    else object[key] = nullptr;
}

json parse_block(const std::vector<Line>& lines, std::size_t& i, int indent,
                 std::vector<SkillManifestIssue>& issues) {
    const bool sequence = i < lines.size() && lines[i].indent == indent &&
                          lines[i].text.rfind("- ", 0) == 0;
    json out = sequence ? json::array() : json::object();
    while (i < lines.size() && lines[i].indent == indent) {
        if (sequence) {
            if (lines[i].text.rfind("- ", 0) != 0) break;
            std::string item = trim(lines[i].text.substr(2));
            if (item.find(':') != std::string::npos) {
                json obj = json::object();
                parse_map_entry(obj, lines, i, indent, item, issues);
                while (i < lines.size() && lines[i].indent > indent) {
                    const int child_indent = lines[i].indent;
                    if (lines[i].text.rfind("- ", 0) == 0) break;
                    parse_map_entry(obj, lines, i, child_indent, lines[i].text, issues);
                }
                out.push_back(std::move(obj));
            } else {
                ++i;
                if (!item.empty()) out.push_back(scalar(item));
                else if (i < lines.size() && lines[i].indent > indent)
                    out.push_back(parse_block(lines, i, lines[i].indent, issues));
                else out.push_back(nullptr);
            }
        } else {
            if (lines[i].text.rfind("- ", 0) == 0) break;
            parse_map_entry(out, lines, i, indent, lines[i].text, issues);
        }
    }
    return out;
}

std::vector<std::string> strings(const json& value) {
    std::vector<std::string> out;
    if (value.is_string()) out.push_back(value.get<std::string>());
    if (value.is_array()) for (const auto& v : value) if (v.is_string()) out.push_back(v.get<std::string>());
    return out;
}

std::string text(const json& object, const char* key) {
    if (object.contains(key) && object[key].is_string()) return object[key].get<std::string>();
    return {};
}

bool boolean(const json& object, const char* key, bool fallback = false) {
    return object.contains(key) && object[key].is_boolean() ? object[key].get<bool>() : fallback;
}

std::string default_resource_id(const std::string& path) {
    std::string id = path;
    const auto slash = id.find_last_of("/\\");
    if (slash != std::string::npos) id = id.substr(slash + 1);
    const auto dot = id.find_last_of('.');
    if (dot != std::string::npos) id.resize(dot);
    return id;
}

SkillResourceDescriptor descriptor(const json& value, SkillResourceType kind) {
    SkillResourceDescriptor d;
    d.kind = kind;
    d.executable = kind == SkillResourceType::Script || kind == SkillResourceType::Cli;
    if (value.is_string()) d.path = value.get<std::string>();
    else if (value.is_object()) {
        d.id = text(value, "id");
        d.path = text(value, "path");
        d.media_type = text(value, "media-type");
        if (d.media_type.empty()) d.media_type = text(value, "media_type");
        d.sha256 = text(value, "sha256");
        d.input_schema = text(value, "input-schema");
        if (d.input_schema.empty()) d.input_schema = text(value, "input_schema");
        d.output_schema = text(value, "output-schema");
        if (d.output_schema.empty()) d.output_schema = text(value, "output_schema");
        d.runtime = text(value, "runtime");
        d.cache_policy = text(value, "cache-policy");
        d.optional = boolean(value, "optional");
        d.executable = boolean(value, "executable", d.executable);
        if (value.contains("size-limit") && value["size-limit"].is_number_unsigned())
            d.size_limit = value["size-limit"].get<std::size_t>();
        if (value.contains("size_limit") && value["size_limit"].is_number_unsigned())
            d.size_limit = value["size_limit"].get<std::size_t>();
    }
    if (d.id.empty()) d.id = default_resource_id(d.path);
    return d;
}

} // namespace

std::string to_string(SkillResourceType kind) {
    static const std::map<SkillResourceType, std::string> values = {
        {SkillResourceType::Script,"script"},{SkillResourceType::Cli,"cli"},
        {SkillResourceType::Reference,"reference"},{SkillResourceType::Tool,"tool"},
        {SkillResourceType::Mcp,"mcp"},{SkillResourceType::Template,"template"},
        {SkillResourceType::Schema,"schema"},{SkillResourceType::Prompt,"prompt"},
        {SkillResourceType::Workflow,"workflow"},{SkillResourceType::Config,"config"},
        {SkillResourceType::Asset,"asset"},{SkillResourceType::Model,"model"},
        {SkillResourceType::Test,"test"},{SkillResourceType::Unknown,"unknown"}};
    return values.at(kind);
}

SkillResourceType skill_resource_type_from_string(const std::string& raw) {
    std::string v = raw;
    if (!v.empty() && v.back() == 's') v.pop_back();
    for (int i = 0; i <= static_cast<int>(SkillResourceType::Test); ++i) {
        auto kind = static_cast<SkillResourceType>(i);
        if (to_string(kind) == v) return kind;
    }
    return SkillResourceType::Unknown;
}

SkillManifestParseResult parse_skill_manifest_yaml(const std::string& yaml) {
    SkillManifestParseResult result;
    auto lines = lex(yaml, result.issues);
    if (lines.empty()) return result;
    std::size_t index = 0;
    json root = parse_block(lines, index, lines.front().indent, result.issues);
    if (!root.is_object()) {
        result.issues.push_back({true,"manifest_not_object","/","manifest must be a mapping","use key: value fields"});
        return result;
    }

    SkillManifest m;
    m.api_version = text(root, "api-version");
    if (m.api_version.empty()) m.api_version = text(root, "api_version");
    m.kind = text(root, "kind");
    m.name = text(root, "name");
    m.legacy_id = text(root, "id");
    m.version = text(root, "version");
    m.description = text(root, "description");
    m.license = text(root, "license");
    m.disable_model_invocation = boolean(root, "disable-model-invocation");
    m.authors = root.contains("authors") ? strings(root["authors"]) : std::vector<std::string>{};
    m.tags = root.contains("tags") ? strings(root["tags"]) : std::vector<std::string>{};
    if (root.contains("trigger_keywords")) m.trigger_keywords = strings(root["trigger_keywords"]);
    if (root.contains("trigger-keywords")) m.trigger_keywords = strings(root["trigger-keywords"]);
    m.legacy_v0 = m.api_version.empty();
    if (m.legacy_v0) { m.api_version = "agent.taskflow/v0"; m.kind = "Skill"; }

    if (root.contains("compatibility") && root["compatibility"].is_object())
        m.compatibility.agent_framework = text(root["compatibility"], "agent-framework");
    if (root.contains("dependencies") && root["dependencies"].is_array()) {
        for (const auto& d : root["dependencies"]) if (d.is_object())
            m.dependencies.push_back({text(d,"name"), text(d,"version"), boolean(d,"optional")});
    }
    if (root.contains("permissions") && root["permissions"].is_object()) {
        const auto& p = root["permissions"];
        if (p.contains("tools")) m.permissions.tools = strings(p["tools"]);
        if (p.contains("network")) m.permissions.network = strings(p["network"]);
        if (p.contains("env")) m.permissions.environment = strings(p["env"]);
        if (p.contains("secrets")) m.permissions.secrets = strings(p["secrets"]);
        if (p.contains("filesystem") && p["filesystem"].is_object()) {
            if (p["filesystem"].contains("read"))
                m.permissions.filesystem_read = strings(p["filesystem"]["read"]);
            if (p["filesystem"].contains("write"))
                m.permissions.filesystem_write = strings(p["filesystem"]["write"]);
        }
    }

    const std::map<std::string, SkillResourceType> resource_types = {
        {"scripts",SkillResourceType::Script},{"cli",SkillResourceType::Cli},
        {"references",SkillResourceType::Reference},{"tools",SkillResourceType::Tool},
        {"mcp",SkillResourceType::Mcp},{"templates",SkillResourceType::Template},
        {"schemas",SkillResourceType::Schema},{"prompts",SkillResourceType::Prompt},
        {"workflows",SkillResourceType::Workflow},{"configs",SkillResourceType::Config},
        {"assets",SkillResourceType::Asset},{"models",SkillResourceType::Model},
        {"tests",SkillResourceType::Test}};
    if (root.contains("resources") && root["resources"].is_object()) {
        for (auto it = root["resources"].begin(); it != root["resources"].end(); ++it) {
            auto found = resource_types.find(it.key());
            if (found == resource_types.end()) {
                result.issues.push_back({true,"unknown_resource_type","/resources/"+it.key(),
                                         "unknown resource type", "use a supported resource type"});
                continue;
            }
            json values = it.value().is_array() ? it.value() : json::array({it.value()});
            for (const auto& value : values) m.resources.push_back(descriptor(value, found->second));
        }
    }
    if (!m.legacy_v0 && root.contains("tests")) {
        json values = root["tests"].is_array() ? root["tests"] : json::array({root["tests"]});
        for (const auto& value : values) m.resources.push_back(descriptor(value, SkillResourceType::Test));
    }
    if (m.legacy_v0) {
        for (const auto& [key, kind] : resource_types) {
            if (!root.contains(key)) continue;
            json values = root[key].is_array() ? root[key] : json::array({root[key]});
            for (const auto& value : values) m.resources.push_back(descriptor(value, kind));
        }
        if (root.contains("allowed-tools")) m.permissions.tools = strings(root["allowed-tools"]);
    }
    const std::unordered_set<std::string> known = {
        "api-version","api_version","kind","name","id","version","description","license",
        "authors","tags","trigger_keywords","trigger-keywords","disable-model-invocation","compatibility",
        "dependencies","permissions","resources","scripts","cli","references","tools","mcp",
        "templates","schemas","prompts","workflows","configs","assets","models","tests","allowed-tools"};
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (known.count(it.key()) == 0U) {
            m.extensions[it.key()] = it.value();
            result.issues.push_back({false,"unknown_manifest_field","/"+it.key(),
                                     "unknown manifest field is preserved", "check the field spelling"});
        }
    }
    result.manifest = std::move(m);
    return result;
}

json skill_manifest_to_json(const SkillManifest& m, bool) {
    json resources = json::object();
    const auto collection_name = [](SkillResourceType kind) {
        if (kind == SkillResourceType::Cli) return std::string("cli");
        if (kind == SkillResourceType::Mcp) return std::string("mcp");
        return to_string(kind) + "s";
    };
    for (const auto& r : m.resources) {
        json value{{"id",r.id},{"path",r.path},{"kind",to_string(r.kind)},
                   {"optional",r.optional},{"executable",r.executable}};
        if (!r.media_type.empty()) value["media_type"] = r.media_type;
        if (!r.sha256.empty()) value["sha256"] = r.sha256;
        if (r.size_limit) value["size_limit"] = *r.size_limit;
        if (!r.input_schema.empty()) value["input_schema"] = r.input_schema;
        if (!r.output_schema.empty()) value["output_schema"] = r.output_schema;
        resources[collection_name(r.kind)].push_back(std::move(value));
    }
    json dependencies = json::array();
    for (const auto& d : m.dependencies) dependencies.push_back({{"name",d.name},{"version",d.version},{"optional",d.optional}});
    return {{"api_version",m.api_version},{"kind",m.kind},{"name",m.name},{"legacy_id",m.legacy_id},
            {"version",m.version},{"description",m.description},{"license",m.license},
            {"authors",m.authors},{"tags",m.tags},{"trigger_keywords",m.trigger_keywords},
            {"compatibility",{{"agent_framework",m.compatibility.agent_framework}}},
            {"dependencies",dependencies},
            {"permissions",{{"tools",m.permissions.tools},{"network",m.permissions.network},
                            {"environment",m.permissions.environment},{"secrets",m.permissions.secrets},
                            {"filesystem",{{"read",m.permissions.filesystem_read},
                                           {"write",m.permissions.filesystem_write}}}}},
            {"resources",resources},{"extensions",m.extensions},{"legacy_v0",m.legacy_v0}};
}

} // namespace agent_framework
