/**
 * @file skill_loader.cpp
 * @brief SkillLoader 实现
 */

#include <agent/skill_loader.hpp>

#include <agent/internal/skill_frontmatter_parse.hpp>

#include <fstream>
#include <sstream>
#include <algorithm>

namespace agent_framework {

namespace {

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
    const auto ent = registry_.get(skill_id);
    if (!ent.has_value()) {
        return std::nullopt;
    }

    const std::filesystem::path& fp = ent->file_path;
    std::error_code ec;
    if (!std::filesystem::exists(fp, ec)) {
        return std::nullopt;
    }

    const std::uintmax_t ts = file_time_stamp(fp);
    const auto cit = cache_.find(skill_id);
    if (cit != cache_.end() && cit->second.second == ts) {
        std::string out = cit->second.first;
        if (out.size() > max_chars) {
            out.resize(max_chars);
        }
        return out;
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
    cache_[skill_id] = {full_body, ts};
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
    auto fail = [&](const std::string& message) -> std::optional<std::string> {
        if (error_out) *error_out = message;
        return std::nullopt;
    };
    const auto entry = registry_.get(skill_id);
    if (!entry) return fail("unknown skill_id");
    if (relative_path.empty() || std::filesystem::path(relative_path).is_absolute()) {
        return fail("resource path must be relative");
    }
    for (const auto& part : std::filesystem::path(relative_path)) {
        if (part == "..") return fail("resource path must not contain ..");
    }

    const std::vector<std::string>* declared = nullptr;
    const char* legacy_prefix = nullptr;
    if (kind == SkillResourceKind::Script) { declared = &entry->scripts; legacy_prefix = "scripts/"; }
    if (kind == SkillResourceKind::Reference) { declared = &entry->references; legacy_prefix = "references/"; }
    if (kind == SkillResourceKind::Cli) { declared = &entry->cli_programs; legacy_prefix = "cli/"; }
    bool authorized = false;
    if (kind == SkillResourceKind::AnyDeclared) {
        const auto contains = [&](const std::vector<std::string>& values) {
            return std::find(values.begin(), values.end(), relative_path) != values.end();
        };
        authorized = contains(entry->scripts) || contains(entry->references) ||
                     contains(entry->cli_programs);
    } else if (declared && !declared->empty()) {
        authorized = std::find(declared->begin(), declared->end(), relative_path) != declared->end();
    } else if (legacy_prefix) {
        authorized = relative_path.rfind(legacy_prefix, 0) == 0;
    }
    if (!authorized) return fail("resource is not declared for requested kind");

    std::error_code ec;
    const auto base = std::filesystem::weakly_canonical(skill_directory(skill_id), ec);
    if (ec || base.empty()) return fail("skill directory resolution failed");
    const auto target = std::filesystem::weakly_canonical(base / relative_path, ec);
    if (ec || !std::filesystem::is_regular_file(target, ec)) return fail("resource is not a regular file");
    const auto relative = std::filesystem::relative(target, base, ec);
    if (ec || relative.empty() || *relative.begin() == "..") return fail("resource escapes skill jail");
    const auto size = std::filesystem::file_size(target, ec);
    if (ec || size > max_bytes) return fail("resource exceeds byte limit");
    std::ifstream input(target, std::ios::binary);
    if (!input) return fail("resource open failed");
    std::string content(static_cast<std::size_t>(size), '\0');
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (!input && !input.eof()) return fail("resource read failed");
    return content;
}

} // namespace agent_framework
