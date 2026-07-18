#include <agent/skills/skill_manager.hpp>

#include <fstream>
#include <regex>

namespace agent_framework {
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
json error_result(std::string code, std::string message) {
    return {{"ok", false}, {"code", std::move(code)}, {"message", std::move(message)}};
}

bool has_error(const SkillRegistrySnapshot& snapshot) {
    for (const auto& diagnostic : snapshot.diagnostics())
        if (diagnostic.severity == SkillDiagnosticSeverity::Error) return true;
    return false;
}

SkillPermissionGrant manifest_grants(const SkillManifest& manifest) {
    SkillPermissionGrant grants;
    grants.tools = manifest.permissions.tools;
    grants.network = manifest.permissions.network;
    grants.environment = manifest.permissions.environment;
    grants.filesystem_read = manifest.permissions.filesystem_read;
    grants.filesystem_write = manifest.permissions.filesystem_write;
    grants.secrets = manifest.permissions.secrets;
    return grants;
}

std::string yaml_double_quoted(const std::string& value) {
    std::string out = "\"";
    for (char c : value) {
        if (c == '\\' || c == '"') out.push_back('\\');
        if (c == '\n') out += "\\n";
        else if (c != '\r') out.push_back(c);
    }
    out.push_back('"');
    return out;
}
} // namespace

SkillManager::SkillManager(std::shared_ptr<SkillRegistry> registry,
                           std::shared_ptr<SkillLoader> loader,
                           std::shared_ptr<SkillRuntime> runtime,
                           fs::path authoring_root)
    : registry_(std::move(registry)), loader_(std::move(loader)), runtime_(std::move(runtime)),
      authoring_root_(std::move(authoring_root)) {}

SkillManager::~SkillManager() { (void)deactivate(); }

void SkillManager::attach_toolbus(std::shared_ptr<ToolBus> toolbus,
                                  SkillMcpClientFactory mcp_factory) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (binding_) throw std::logic_error("cannot replace ToolBus while a Skill is active");
    toolbus_ = std::move(toolbus);
    capabilities_ = toolbus_ ? std::make_shared<SkillCapabilityRuntime>(
        registry_, loader_, runtime_, toolbus_, std::move(mcp_factory)) : nullptr;
}

json SkillManager::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto snapshot = registry_->snapshot();
    json roots = json::array();
    for (const auto& root : registry_->roots()) roots.push_back(root.string());
    json diagnostics = json::array();
    for (const auto& diagnostic : snapshot.diagnostics()) {
        diagnostics.push_back({
            {"severity", diagnostic.severity == SkillDiagnosticSeverity::Error ? "error" : "warning"},
            {"code", diagnostic.code}, {"path", diagnostic.path.string()},
            {"message", diagnostic.message}});
    }
    json tools = json::array();
    if (binding_) for (const auto& tool : binding_->registered_tools()) tools.push_back(tool);
    return {{"ok", true}, {"generation", snapshot.generation()},
            {"skills", snapshot.entries().size()}, {"diagnosticCount", diagnostics.size()},
            {"diagnostics", diagnostics},
            {"roots", roots}, {"authoringRoot", authoring_root_.string()},
            {"activeSkill", active_skill_id_.empty() ? json(nullptr) : json(active_skill_id_)},
            {"registeredTools", tools}, {"lastReload", last_reload_}};
}

json SkillManager::list() const {
    const auto snapshot = registry_->snapshot();
    json skills = json::array();
    for (const auto& entry : snapshot.entries())
        skills.push_back({{"id", entry.id}, {"description", entry.description},
                          {"version", entry.version}, {"path", entry.file_path.string()}});
    return {{"ok", true}, {"generation", snapshot.generation()}, {"skills", skills}};
}

json SkillManager::reload() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (binding_) return error_result("skill_active", "deactivate the current Skill before reload");
    const auto before = registry_->snapshot();
    try {
        registry_->scan_or_reload();
        const auto after = registry_->snapshot();
        if (has_error(after)) {
            registry_->publish(before.entries(), before.diagnostics());
            last_reload_ = error_result("skill_reload_invalid", "reload contained error diagnostics; previous snapshot restored");
            return last_reload_;
        }
        last_reload_ = {{"ok", true}, {"previousGeneration", before.generation()},
                        {"generation", after.generation()}, {"skills", after.entries().size()}};
        return last_reload_;
    } catch (const std::exception& e) {
        last_reload_ = error_result("skill_reload_failed", e.what());
        return last_reload_;
    }
}

json SkillManager::validate(const std::string& skill_id) const {
    const auto snapshot = registry_->snapshot();
    const auto entry = snapshot.get(skill_id);
    if (!entry) return error_result("skill_not_found", "Skill is not indexed: " + skill_id);
    if (!snapshot.get_manifest(skill_id))
        return error_result("skill_manifest_invalid", "Skill manifest is unavailable: " + skill_id);
    return {{"ok", true}, {"id", entry->id}, {"version", entry->version},
            {"packageDigest", entry->package_digest}, {"generation", snapshot.generation()}};
}

bool SkillManager::valid_skill_id(const std::string& id) {
    static const std::regex pattern("^[a-z0-9][a-z0-9_-]{0,63}$");
    return std::regex_match(id, pattern);
}

json SkillManager::create(const std::string& skill_id, const std::string& description) {
    if (!valid_skill_id(skill_id)) return error_result("skill_id_invalid", "Skill id must match [a-z0-9][a-z0-9_-]{0,63}");
    if (authoring_root_.empty()) return error_result("skill_authoring_disabled", "AGENT_SKILL_AUTHORING_DIR is not configured");
    std::error_code ec;
    fs::create_directories(authoring_root_, ec);
    if (ec) return error_result("skill_create_failed", ec.message());
    const fs::path root = fs::weakly_canonical(authoring_root_, ec);
    const fs::path package = root / skill_id;
    if (fs::exists(package)) return error_result("skill_exists", "Skill package already exists: " + skill_id);
    if (!fs::create_directory(package, ec) || ec) return error_result("skill_create_failed", ec.message());
    const fs::path temporary = package / "SKILL.md.tmp";
    const fs::path target = package / "SKILL.md";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) { fs::remove_all(package, ec); return error_result("skill_create_failed", "cannot create SKILL.md"); }
        out << "---\napi-version: agent.taskflow/v1\nkind: Skill\nname: " << skill_id
            << "\nversion: 0.1.0\ndescription: "
            << yaml_double_quoted(description.empty() ? "New Skill" : description)
            << "\npermissions:\n  tools: []\nresources: {}\n---\n\n# " << skill_id
            << "\n\nDescribe when and how this Skill should be used.\n";
    }
    fs::rename(temporary, target, ec);
    if (ec) { fs::remove_all(package, ec); return error_result("skill_create_failed", "cannot publish SKILL.md"); }
    return {{"ok", true}, {"id", skill_id}, {"path", package.string()},
            {"reloadRequired", true}};
}

json SkillManager::activate(const std::string& skill_id, SkillInvocationContext context) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (binding_) return error_result("skill_active", "deactivate the current Skill first");
    if (!capabilities_) return error_result("skill_runtime_unavailable", "Skill capability runtime has no ToolBus");
    const auto snapshot = registry_->snapshot();
    auto entry = snapshot.get(skill_id);
    auto manifest = snapshot.get_manifest(skill_id);
    if (!entry || !manifest) return error_result("skill_not_found", "Skill is not indexed: " + skill_id);
    if (context.grants.tools.empty() && context.grants.network.empty() &&
        context.grants.environment.empty() && context.grants.filesystem_read.empty() &&
        context.grants.filesystem_write.empty() && context.grants.secrets.empty())
        context.grants = manifest_grants(*manifest);
    context.skill_id = skill_id;
    context.skill_version = entry->version;
    context.package_digest = entry->package_digest;
    context.registry_generation = snapshot.generation();
    auto result = capabilities_->bind_snapshot(*entry, manifest, std::move(context));
    if (!result.ok()) return {{"ok", false}, {"code", "skill_bind_failed"}, {"details", result.error}};
    binding_ = std::move(result.binding);
    active_skill_id_ = skill_id;
    return {{"ok", true}, {"activeSkill", skill_id},
            {"generation", snapshot.generation()}, {"registeredTools", binding_->registered_tools()}};
}

json SkillManager::deactivate() {
    std::shared_ptr<SkillCapabilityBinding> old;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        old = std::move(binding_);
        active_skill_id_.clear();
    }
    if (old) old->close();
    return {{"ok", true}, {"activeSkill", nullptr}};
}

std::string SkillManager::active_skill_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_skill_id_;
}

} // namespace agent_framework
