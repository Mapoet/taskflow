/**
 * @file skill_registry.cpp
 * @brief SkillRegistry 实现
 */

#include <agent/skill_registry.hpp>

#include <agent/internal/skill_frontmatter_parse.hpp>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_set>

namespace agent_framework {

namespace {

constexpr int kCanonical_full_match_bonus = 2;
constexpr int kCanonical_part_bonus_cap = 3;

void score_canonical_in_user_text(const std::string& hay_lower, const std::string& canon_lower,
                                  int& score) {
    if (canon_lower.empty()) {
        return;
    }
    if (hay_lower.find(canon_lower) != std::string::npos) {
        score += kCanonical_full_match_bonus;
    }
    int part_bonus = 0;
    std::size_t start = 0;
    while (start <= canon_lower.size()) {
        const std::size_t dash = canon_lower.find('-', start);
        const std::string part = (dash == std::string::npos)
                                       ? canon_lower.substr(start)
                                       : canon_lower.substr(start, dash - start);
        if (part.size() >= 3U && hay_lower.find(part) != std::string::npos) {
            ++part_bonus;
            if (part_bonus >= kCanonical_part_bonus_cap) {
                break;
            }
        }
        if (dash == std::string::npos) {
            break;
        }
        start = dash + 1U;
    }
    score += part_bonus;
}


bool env_skill_router_off() {
    const char* v = std::getenv("AGENT_SKILL_ROUTER");
    if (!v || !*v) {
        return false;
    }
    std::string s(v);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s == "off" || s == "0" || s == "false";
}

std::string lower_copy(std::string_view sv) {
    std::string o;
    o.reserve(sv.size());
    for (unsigned char c : sv) {
        o.push_back(static_cast<char>(std::tolower(c)));
    }
    return o;
}

std::filesystem::path normalize_root(std::filesystem::path p) {
    std::error_code ec;
    std::filesystem::path c = std::filesystem::weakly_canonical(p, ec);
    return ec ? std::filesystem::absolute(p) : std::move(c);
}

} // namespace

SkillRegistry::SkillRegistry(std::filesystem::path root_directory) {
    roots_ = {normalize_root(std::move(root_directory))};
    primary_root_ = roots_.front();
}

SkillRegistry::SkillRegistry(std::vector<std::filesystem::path> root_directories) {
    roots_.clear();
    if (root_directories.empty()) {
        primary_root_.clear();
        return;
    }
    roots_.reserve(root_directories.size());
    for (auto& p : root_directories) {
        roots_.push_back(normalize_root(std::move(p)));
    }
    primary_root_ = roots_.front();
}

void SkillRegistry::scan_one_root(const std::filesystem::path& scan_root,
                                  std::unordered_set<std::string>& seen_ids) {
    if (!std::filesystem::exists(scan_root) || !std::filesystem::is_directory(scan_root)) {
        return;
    }

    std::error_code d_ec;
    for (const std::filesystem::directory_entry& de :
         std::filesystem::directory_iterator(scan_root, d_ec)) {
        if (d_ec) {
            std::clog << "[SkillRegistry] directory_iterator " << scan_root << ": " << d_ec.message()
                      << '\n';
            break;
        }
        if (!de.is_directory()) {
            continue;
        }

        const std::filesystem::path skill_dir = de.path();
        const std::filesystem::path skill_md = skill_dir / "SKILL.md";
        std::error_code f_ec;
        if (!std::filesystem::is_regular_file(skill_md, f_ec)) {
            continue;
        }

        std::ifstream f(skill_md);
        if (!f) {
            continue;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        const std::string content = ss.str();

        const internal::SplitFrontmatterResult sp = internal::split_skill_file_content(content);
        if (!sp.ok) {
            std::clog << "[SkillRegistry] skip (no frontmatter): " << skill_md << '\n';
            continue;
        }

        auto parsed = internal::parse_skill_frontmatter_yaml(sp.yaml_inner, nullptr);
        if (!parsed.has_value()) {
            std::clog << "[SkillRegistry] skip (empty YAML block): " << skill_md << '\n';
            continue;
        }

        SkillIndexEntry e = std::move(*parsed);
        const std::string folder_name = skill_dir.filename().string();

        if (!e.name.empty() && !e.yaml_id.empty() && e.name != e.yaml_id) {
            std::clog << "[SkillRegistry] warn: id/name mismatch in " << skill_md
                      << " (canonical from name)\n";
        }
        if (!e.name.empty() && e.name != folder_name) {
            std::clog << "[SkillRegistry] warn: name \"" << e.name << "\" != directory \"" << folder_name
                      << "\" in " << skill_md << '\n';
        }

        std::string canonical;
        if (!e.name.empty()) {
            canonical = e.name;
        } else if (!e.yaml_id.empty()) {
            canonical = e.yaml_id;
        } else {
            canonical = folder_name;
        }
        e.id = std::move(canonical);

        std::error_code c_md;
        std::error_code c_dir;
        const std::filesystem::path canon_md =
            std::filesystem::weakly_canonical(skill_md, c_md);
        const std::filesystem::path canon_dir =
            std::filesystem::weakly_canonical(skill_dir, c_dir);
        e.file_path = c_md ? skill_md : canon_md;
        if (!c_dir) {
            e.script_jail = canon_dir;
        }

        if (seen_ids.count(e.id) != 0U) {
            std::clog << "[SkillRegistry] duplicate canonical id \"" << e.id << "\" skipped: " << skill_md
                      << '\n';
            continue;
        }

        seen_ids.insert(e.id);
        entries_.push_back(std::move(e));
    }
}

void SkillRegistry::scan_or_reload() {
    entries_.clear();
    if (roots_.empty()) {
        return;
    }

    std::unordered_set<std::string> seen_ids;
    for (const std::filesystem::path& r : roots_) {
        scan_one_root(r, seen_ids);
    }

    std::sort(entries_.begin(), entries_.end(),
              [](const SkillIndexEntry& a, const SkillIndexEntry& b) { return a.id < b.id; });
}

std::optional<SkillIndexEntry> SkillRegistry::get(std::string_view skill_id) const {
    for (const auto& e : entries_) {
        if (e.id == skill_id) {
            return e;
        }
    }
    return std::nullopt;
}

std::optional<std::string> SkillRegistry::match(std::string_view user_text) const {
    if (env_skill_router_off() || entries_.empty()) {
        return std::nullopt;
    }

    const std::string hay = lower_copy(user_text);
    int best = -1;
    std::optional<std::string> best_id;

    for (const auto& e : entries_) {
        if (e.disable_model_invocation) {
            continue;
        }
        int score = 0;
        score_canonical_in_user_text(hay, lower_copy(e.id), score);
        for (const auto& kw : e.trigger_keywords) {
            if (kw.empty()) {
                continue;
            }
            const std::string k = lower_copy(kw);
            if (hay.find(k) != std::string::npos) {
                ++score;
            }
        }
        for (const auto& t : e.tags) {
            if (t.empty()) {
                continue;
            }
            const std::string k = lower_copy(t);
            if (hay.find(k) != std::string::npos) {
                ++score;
            }
        }
        if (score <= 0) {
            continue;
        }
        if (score > best) {
            best = score;
            best_id = e.id;
        } else if (score == best && best_id.has_value() && e.id < *best_id) {
            best_id = e.id;
        }
    }

    return best_id;
}

} // namespace agent_framework
