/**
 * @file skill_loader.cpp
 * @brief SkillLoader 实现
 */

#include <agent/skill_loader.hpp>

#include <agent/internal/skill_frontmatter_parse.hpp>

#include <fstream>
#include <sstream>

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
    return std::filesystem::weakly_canonical(registry_.root() / skill_id);
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

} // namespace agent_framework
