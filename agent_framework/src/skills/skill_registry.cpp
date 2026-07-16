/**
 * @file skill_registry.cpp
 * @brief SkillRegistry 实现
 */

#include <agent/skills/skill_registry.hpp>

#include <agent/internal/skill_frontmatter_parse.hpp>
#include <agent/skills/skill_manifest.hpp>

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

bool safe_skill_id(std::string_view id) {
    if (id.empty() || id.size() > 128) return false;
    for (unsigned char c : id) {
        if (!std::isalnum(c) && c != '-' && c != '_' && c != '.') return false;
    }
    return id != "." && id != "..";
}

bool safe_relative_resource(const std::string& value) {
    if (value.empty()) return false;
    const std::filesystem::path path(value);
    if (path.is_absolute()) return false;
    for (const auto& part : path) if (part == "..") return false;
    return true;
}

} // namespace

SkillRegistry::SkillRegistry(std::filesystem::path root_directory) {
    roots_ = {normalize_root(std::move(root_directory))};
    primary_root_ = roots_.front();
    std::atomic_store(&state_, std::make_shared<const SkillRegistryState>());
}

SkillRegistry::SkillRegistry(std::vector<std::filesystem::path> root_directories) {
    roots_.clear();
    if (root_directories.empty()) {
        primary_root_.clear();
        std::atomic_store(&state_, std::make_shared<const SkillRegistryState>());
        return;
    }
    roots_.reserve(root_directories.size());
    for (auto& p : root_directories) {
        roots_.push_back(normalize_root(std::move(p)));
    }
    primary_root_ = roots_.front();
    std::atomic_store(&state_, std::make_shared<const SkillRegistryState>());
}

void SkillRegistry::scan_one_root(const std::filesystem::path& scan_root,
                                  std::unordered_set<std::string>& seen_ids,
                                  std::vector<SkillIndexEntry>& entries,
                                  std::vector<SkillDiagnostic>& diagnostics) const {
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
            diagnostics.push_back({SkillDiagnosticSeverity::Warning, "missing_frontmatter",
                                    skill_md, "SKILL.md has no YAML frontmatter", "/", "add YAML frontmatter"});
            std::clog << "[SkillRegistry] skip (no frontmatter): " << skill_md << '\n';
            continue;
        }

        auto parsed_manifest = parse_skill_manifest_yaml(sp.yaml_inner);
        if (!parsed_manifest.manifest.has_value()) {
            std::clog << "[SkillRegistry] skip (empty YAML block): " << skill_md << '\n';
            continue;
        }
        auto manifest = std::move(*parsed_manifest.manifest);
        auto validation = validate_skill_manifest(manifest, skill_dir);
        parsed_manifest.issues.insert(parsed_manifest.issues.end(), validation.begin(), validation.end());
        bool manifest_error = false;
        for (const auto& issue : parsed_manifest.issues) {
            diagnostics.push_back({issue.error ? SkillDiagnosticSeverity::Error : SkillDiagnosticSeverity::Warning,
                                    issue.code, skill_md, issue.message, issue.location, issue.suggestion});
            manifest_error = manifest_error || issue.error;
        }
        if (manifest_error) continue;

        SkillIndexEntry e;
        e.name = manifest.name;
        e.yaml_id = manifest.legacy_id;
        e.description = manifest.description;
        e.version = manifest.version;
        e.license = manifest.license;
        e.trigger_keywords = manifest.trigger_keywords;
        e.tags = manifest.tags;
        e.disable_model_invocation = manifest.disable_model_invocation;
        e.allowed_tools = manifest.permissions.tools;
        for (const auto& resource : manifest.resources) {
            if (resource.kind == SkillResourceType::Script) e.scripts.push_back(resource.path);
            if (resource.kind == SkillResourceType::Reference) e.references.push_back(resource.path);
            if (resource.kind == SkillResourceType::Cli) e.cli_programs.push_back(resource.path);
        }
        e.manifest = std::make_shared<SkillManifest>(std::move(manifest));
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
        if (!safe_skill_id(e.id)) {
            diagnostics.push_back({SkillDiagnosticSeverity::Error, "invalid_skill_id", skill_md,
                                    "canonical skill id must match [A-Za-z0-9_.-]{1,128}", "/name", {}});
            continue;
        }
        if (e.description.empty()) {
            diagnostics.push_back({SkillDiagnosticSeverity::Warning, "missing_description",
                                    skill_md, "skill description is empty", "/description", {}});
        }

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
            diagnostics.push_back({SkillDiagnosticSeverity::Error, "duplicate_skill_id", skill_md,
                                    "duplicate canonical skill id: " + e.id, "/name", {}});
            std::clog << "[SkillRegistry] duplicate canonical id \"" << e.id << "\" skipped: " << skill_md
                      << '\n';
            continue;
        }

        auto validate_resources = [&](const std::vector<std::string>& values, const char* kind) {
            for (const auto& value : values) {
                if (!safe_relative_resource(value)) {
                    diagnostics.push_back({SkillDiagnosticSeverity::Error,
                                            "invalid_resource_path", skill_md,
                                            std::string(kind) + " path is not jail-relative: " + value,
                                            "/resources", {}});
                }
            }
        };
        validate_resources(e.scripts, "script");
        validate_resources(e.references, "reference");
        validate_resources(e.cli_programs, "cli");

        seen_ids.insert(e.id);
        entries.push_back(std::move(e));
    }
}

void SkillRegistry::scan_or_reload() {
    std::vector<SkillIndexEntry> entries;
    std::vector<SkillDiagnostic> diagnostics;
    if (roots_.empty()) {
        publish({}, {});
        return;
    }

    std::unordered_set<std::string> seen_ids;
    for (const std::filesystem::path& r : roots_) {
        scan_one_root(r, seen_ids, entries, diagnostics);
    }

    std::sort(entries.begin(), entries.end(),
              [](const SkillIndexEntry& a, const SkillIndexEntry& b) { return a.id < b.id; });
    const bool candidate_valid = std::none_of(
        diagnostics.begin(), diagnostics.end(), [](const SkillDiagnostic& diagnostic) {
            return diagnostic.severity == SkillDiagnosticSeverity::Error;
        });
    if (candidate_valid) {
        publish(std::move(entries), std::move(diagnostics));
        return;
    }
    std::lock_guard<std::mutex> lock(reload_mutex_);
    const auto current = snapshot();
    auto next = std::make_shared<SkillRegistryState>();
    next->entries = current.generation() == 0
        ? std::move(entries) : std::vector<SkillIndexEntry>(current.entries());
    next->diagnostics = std::move(diagnostics);
    next->generation = current.generation() == 0 ? 1 : current.generation();
    std::atomic_store_explicit(
        &state_, std::shared_ptr<const SkillRegistryState>(std::move(next)),
        std::memory_order_release);
}

bool SkillRegistry::valid() const {
    return snapshot().valid();
}

std::optional<SkillIndexEntry> SkillRegistry::get(std::string_view skill_id) const {
    return snapshot().get(skill_id);
}

std::shared_ptr<const SkillManifest> SkillRegistry::get_manifest(std::string_view skill_id) const {
    return snapshot().get_manifest(skill_id);
}

std::optional<std::string> SkillRegistry::match(std::string_view user_text) const {
    return snapshot().match(user_text);
}

SkillRegistrySnapshot SkillRegistry::snapshot() const {
    return SkillRegistrySnapshot(std::atomic_load_explicit(&state_, std::memory_order_acquire));
}

void SkillRegistry::publish(std::vector<SkillIndexEntry> entries,
                            std::vector<SkillDiagnostic> diagnostics) {
    std::lock_guard<std::mutex> lock(reload_mutex_);
    std::sort(entries.begin(), entries.end(),
              [](const SkillIndexEntry& lhs, const SkillIndexEntry& rhs) {
                  return lhs.id < rhs.id;
              });
    auto current = std::atomic_load_explicit(&state_, std::memory_order_acquire);
    auto next = std::make_shared<SkillRegistryState>();
    next->entries = std::move(entries);
    next->diagnostics = std::move(diagnostics);
    next->generation = current ? current->generation + 1 : 1;
    std::atomic_store_explicit(
        &state_, std::shared_ptr<const SkillRegistryState>(std::move(next)),
        std::memory_order_release);
}

const std::vector<SkillIndexEntry>& SkillRegistrySnapshot::entries() const {
    static const std::vector<SkillIndexEntry> empty;
    return state_ ? state_->entries : empty;
}

const std::vector<SkillDiagnostic>& SkillRegistrySnapshot::diagnostics() const {
    static const std::vector<SkillDiagnostic> empty;
    return state_ ? state_->diagnostics : empty;
}

std::uint64_t SkillRegistrySnapshot::generation() const noexcept {
    return state_ ? state_->generation : 0;
}

bool SkillRegistrySnapshot::valid() const {
    return std::none_of(diagnostics().begin(), diagnostics().end(), [](const SkillDiagnostic& d) {
        return d.severity == SkillDiagnosticSeverity::Error;
    });
}

std::optional<SkillIndexEntry> SkillRegistrySnapshot::get(std::string_view skill_id) const {
    const auto found = std::lower_bound(
        entries().begin(), entries().end(), skill_id,
        [](const SkillIndexEntry& entry, std::string_view id) { return entry.id < id; });
    return found != entries().end() && found->id == skill_id
        ? std::optional<SkillIndexEntry>(*found) : std::nullopt;
}

std::shared_ptr<const SkillManifest> SkillRegistrySnapshot::get_manifest(
    std::string_view skill_id) const {
    const auto entry = get(skill_id);
    return entry ? entry->manifest : nullptr;
}

std::optional<std::string> SkillRegistrySnapshot::match(std::string_view user_text) const {
    if (env_skill_router_off() || entries().empty()) {
        return std::nullopt;
    }

    const std::string hay = lower_copy(user_text);
    int best = -1;
    std::optional<std::string> best_id;

    for (const auto& e : entries()) {
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
