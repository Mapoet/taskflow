/**
 * @file skill_loader.cpp
 * @brief SkillLoader 实现
 */

#include <agent/skill_loader.hpp>
#include <agent/skill_manifest.hpp>

#include <agent/internal/skill_frontmatter_parse.hpp>

#include <fstream>
#include <sstream>
#include <algorithm>

namespace agent_framework {

namespace {

SkillResourceType manifest_kind(SkillResourceKind kind) {
    switch (kind) {
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

const char* legacy_directory(SkillResourceKind kind) {
    switch (kind) {
    case SkillResourceKind::Script: return "scripts/";
    case SkillResourceKind::Cli: return "cli/";
    case SkillResourceKind::Reference: return "references/";
    case SkillResourceKind::Tool: return "tools/";
    case SkillResourceKind::Mcp: return "mcp/";
    case SkillResourceKind::Template: return "templates/";
    case SkillResourceKind::Schema: return "schemas/";
    case SkillResourceKind::Prompt: return "prompts/";
    case SkillResourceKind::Workflow: return "workflows/";
    case SkillResourceKind::Config: return "configs/";
    case SkillResourceKind::Asset: return "assets/";
    case SkillResourceKind::Model: return "models/";
    case SkillResourceKind::Test: return "tests/";
    case SkillResourceKind::AnyDeclared: return nullptr;
    }
    return nullptr;
}

std::uintmax_t file_time_stamp(const std::filesystem::path& p) {
    std::error_code ec;
    const auto ft = std::filesystem::last_write_time(p, ec);
    if (ec) {
        return 0;
    }
    return static_cast<std::uintmax_t>(ft.time_since_epoch().count());
}

} // namespace

SkillLoader::SkillLoader(const SkillRegistry& registry) : registry_(registry) {}

std::filesystem::path SkillLoader::skill_directory(const std::string& skill_id) const {
    const auto ent = registry_.get(skill_id);
    if (!ent.has_value()) {
        return {};
    }
    std::error_code ec;
    if (ent->script_jail.has_value()) {
        return std::filesystem::weakly_canonical(*ent->script_jail, ec);
    }
    return std::filesystem::weakly_canonical(ent->file_path.parent_path(), ec);
}

std::optional<std::string> SkillLoader::load_instructions(const std::string& skill_id,
                                                          std::size_t max_chars) const {
    const auto registry_snapshot = registry_.snapshot();
    const auto ent = registry_snapshot.get(skill_id);
    if (!ent.has_value()) {
        return std::nullopt;
    }

    const std::filesystem::path& fp = ent->file_path;
    std::error_code ec;
    if (!std::filesystem::exists(fp, ec)) {
        return std::nullopt;
    }

    const std::string identity = ent->package_digest.empty()
        ? std::to_string(file_time_stamp(fp)) : ent->package_digest;
    const std::string cache_key = skill_id + "@" + identity;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        const auto cit = cache_.find(cache_key);
        if (cit != cache_.end() && cit->second.second == identity) {
            std::string out = cit->second.first;
            if (out.size() > max_chars) out.resize(max_chars);
            return out;
        }
    }

    std::ifstream f(fp);
    if (!f) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const internal::SplitFrontmatterResult sp = internal::split_skill_file_content(ss.str());
    if (!sp.ok) {
        return std::nullopt;
    }
    std::string full_body = sp.body;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        cache_[cache_key] = {full_body, identity};
    }
    if (full_body.size() > max_chars) {
        full_body.resize(max_chars);
    }
    return full_body;
}

std::optional<std::string> SkillLoader::load_resource(const std::string& skill_id,
                                                      const std::string& relative_path,
                                                      SkillResourceKind kind,
                                                      std::size_t max_bytes,
                                                      std::string* error_out) const {
    const auto registry_snapshot = registry_.snapshot();
    const auto entry = registry_snapshot.get(skill_id);
    if (!entry) {
        if (error_out) *error_out = "unknown skill_id";
        return std::nullopt;
    }
    return load_resource_snapshot(*entry, entry->manifest, relative_path, kind, max_bytes,
                                  error_out);
}

std::optional<std::string> SkillLoader::load_resource_snapshot(
    const SkillIndexEntry& entry, std::shared_ptr<const SkillManifest> manifest,
    const std::string& relative_path, SkillResourceKind kind, std::size_t max_bytes,
    std::string* error_out) const {
    auto fail = [&](const std::string& message) -> std::optional<std::string> {
        if (error_out) *error_out = message;
        return std::nullopt;
    };
    if (relative_path.empty() || std::filesystem::path(relative_path).is_absolute()) {
        return fail("resource path must be relative");
    }
    for (const auto& part : std::filesystem::path(relative_path)) {
        if (part == "..") return fail("resource path must not contain ..");
    }

    bool authorized = false;
    std::string resource_id;
    SkillResourceType resource_type = SkillResourceType::Unknown;
    if (manifest) {
        const SkillResourceType requested = manifest_kind(kind);
        for (const auto& resource : manifest->resources) {
            if (resource.path == relative_path &&
                (kind == SkillResourceKind::AnyDeclared || resource.kind == requested)) {
                authorized = true;
                resource_id = resource.id;
                resource_type = resource.kind;
                break;
            }
        }
        if (!authorized && manifest->legacy_v0 && kind != SkillResourceKind::AnyDeclared) {
            const char* prefix = legacy_directory(kind);
            bool kind_declared = false;
            for (const auto& resource : manifest->resources)
                kind_declared = kind_declared || resource.kind == requested;
            authorized = !kind_declared && prefix && relative_path.rfind(prefix, 0) == 0;
            if(authorized) resource_type = requested;
        }
    }
    if (!authorized) return fail("resource is not declared for requested kind");

    std::error_code ec;
    const auto package = entry.script_jail.value_or(entry.file_path.parent_path());
    const auto base = std::filesystem::weakly_canonical(package, ec);
    if (ec || base.empty()) return fail("skill directory resolution failed");
    const auto target = std::filesystem::weakly_canonical(base / relative_path, ec);
    if (ec || !std::filesystem::is_regular_file(target, ec)) return fail("resource is not a regular file");
    const auto relative = std::filesystem::relative(target, base, ec);
    if (ec || relative.empty() || *relative.begin() == "..") return fail("resource escapes skill jail");
    const auto size = std::filesystem::file_size(target, ec);
    if (ec || size > max_bytes) return fail("resource exceeds byte limit");
    const auto digest = entry.resource_digests.find(resource_id);
    const std::string resource_identity = digest != entry.resource_digests.end()
        ? entry.package_digest + ":" + digest->second
        : std::to_string(file_time_stamp(target));
    const std::string cache_key = "resource:" + entry.id + ":" + relative_path + "@" +
                                  resource_identity;
    const bool cacheable = resource_type != SkillResourceType::Asset &&
                           resource_type != SkillResourceType::Model;
    if(cacheable) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        const auto cached = cache_.find(cache_key);
        if (cached != cache_.end() && cached->second.second == resource_identity)
            return cached->second.first;
    }
    std::ifstream input(target, std::ios::binary);
    if (!input) return fail("resource open failed");
    std::string content(static_cast<std::size_t>(size), '\0');
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (!input && !input.eof()) return fail("resource read failed");
    if(cacheable) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        cache_[cache_key] = {content, resource_identity};
    }
    return content;
}

} // namespace agent_framework
