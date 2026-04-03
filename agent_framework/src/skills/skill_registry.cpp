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

    for (const std::filesystem::directory_entry& de :
         std::filesystem::recursive_directory_iterator(scan_root)) {
        if (!de.is_regular_file()) {
            continue;
        }
        const auto& p = de.path();
        if (p.extension() != ".md") {
            continue;
        }
        const std::string stem = p.stem().string();
        if (stem.size() < 7 || stem.compare(stem.size() - 6, 6, ".skill") != 0) {
            continue;
        }

        std::ifstream f(p);
        if (!f) {
            continue;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        const std::string content = ss.str();

        const internal::SplitFrontmatterResult sp = internal::split_skill_file_content(content);
        if (!sp.ok) {
            std::clog << "[SkillRegistry] skip (no frontmatter): " << p << '\n';
            continue;
        }

        std::string err;
        auto parsed = internal::parse_skill_frontmatter_yaml(sp.yaml_inner, &err);
        if (!parsed.has_value()) {
            std::clog << "[SkillRegistry] skip " << p << ": " << err << '\n';
            continue;
        }

        SkillIndexEntry e = std::move(*parsed);
        e.file_path = std::filesystem::weakly_canonical(p);

        if (seen_ids.count(e.id) != 0U) {
            std::clog << "[SkillRegistry] duplicate id \"" << e.id << "\" skipped: " << p << '\n';
            continue;
        }

        std::error_code ec;
        const std::filesystem::path jail_candidate = scan_root / e.id;
        if (std::filesystem::is_directory(jail_candidate, ec)) {
            e.script_jail = std::filesystem::weakly_canonical(jail_candidate, ec);
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
        int score = 0;
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
