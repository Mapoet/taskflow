#include <agent/skills/skill_lifecycle.hpp>

#include <agent/internal/skill_frontmatter_parse.hpp>
#include <agent/skills/skill_archive.hpp>
#include <agent/skills/skill_package_gate.hpp>

#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
#include <openssl/evp.h>
#endif

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <functional>
#include <fstream>
#include <iomanip>
#include <limits>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace agent_framework {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

json failure(const char* code, const std::string& message,
             json details = json::object()) {
    return {{"error", message}, {"code", code}, {"details", std::move(details)}};
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::vector<std::string> split(const std::string& value, char delimiter) {
    std::vector<std::string> out;
    std::istringstream stream(value);
    std::string item;
    while (std::getline(stream, item, delimiter)) out.push_back(trim(item));
    return out;
}

bool numeric_identifier(const std::string& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isdigit(c) != 0;
    });
}

bool semver_identifier(const std::string& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '-';
    });
}

bool valid_semver_identifiers(const std::string& value) {
    if (value.empty() || value.front() == '.' || value.back() == '.' ||
        value.find("..") != std::string::npos) return false;
    const auto identifiers = split(value, '.');
    return !identifiers.empty() &&
        std::all_of(identifiers.begin(), identifiers.end(), semver_identifier);
}

bool sha256_identity(const std::string& value) {
    return value.size() == 64 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isxdigit(c) != 0;
        });
}

bool parse_number(const std::string& value, std::uint64_t& output) {
    if (!numeric_identifier(value) || (value.size() > 1 && value.front() == '0')) return false;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), output);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}

int compare_identifiers(const std::string& lhs, const std::string& rhs) {
    const bool left_number = numeric_identifier(lhs);
    const bool right_number = numeric_identifier(rhs);
    if (left_number && right_number) {
        if (lhs.size() != rhs.size()) return lhs.size() < rhs.size() ? -1 : 1;
        return lhs == rhs ? 0 : (lhs < rhs ? -1 : 1);
    }
    if (left_number != right_number) return left_number ? -1 : 1;
    return lhs == rhs ? 0 : (lhs < rhs ? -1 : 1);
}

std::optional<SkillManifest> read_manifest(const fs::path& package, std::string* error) {
    std::ifstream input(package / "SKILL.md", std::ios::binary);
    if (!input) {
        if (error) *error = "SKILL.md is unavailable";
        return std::nullopt;
    }
    std::ostringstream content;
    content << input.rdbuf();
    auto frontmatter = internal::split_skill_file_content(content.str());
    if (!frontmatter.ok) {
        if (error) *error = "SKILL.md has no frontmatter";
        return std::nullopt;
    }
    auto parsed = parse_skill_manifest_yaml(frontmatter.yaml_inner);
    if (!parsed.manifest) {
        if (error) *error = "Skill manifest cannot be parsed";
        return std::nullopt;
    }
    auto issues = validate_skill_manifest(*parsed.manifest, package);
    parsed.issues.insert(parsed.issues.end(), issues.begin(), issues.end());
    for (const auto& issue : parsed.issues) {
        if (issue.error) {
            if (error) *error = issue.code + ": " + issue.message;
            return std::nullopt;
        }
    }
    return std::move(*parsed.manifest);
}

std::vector<fs::path> package_files(const fs::path& package, std::string* error) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(package, fs::directory_options::none, ec), end;
         it != end; it.increment(ec)) {
        if (ec) {
            if (error) *error = "package traversal failed: " + ec.message();
            return {};
        }
        const auto status = it->symlink_status(ec);
        if (ec || fs::is_symlink(status) || (!fs::is_directory(status) && !fs::is_regular_file(status))) {
            if (error) *error = "package contains a symlink or special file";
            return {};
        }
        if (fs::is_regular_file(status)) files.push_back(fs::relative(it->path(), package, ec));
        if (ec) {
            if (error) *error = "package path normalization failed";
            return {};
        }
    }
    std::sort(files.begin(), files.end(), [](const fs::path& lhs, const fs::path& rhs) {
        return lhs.generic_string() < rhs.generic_string();
    });
    return files;
}

#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
class Sha256 {
public:
    Sha256() : context_(EVP_MD_CTX_new()) {
        if (!context_ || EVP_DigestInit_ex(context_, EVP_sha256(), nullptr) != 1)
            throw std::runtime_error("SHA-256 initialization failed");
    }
    ~Sha256() { EVP_MD_CTX_free(context_); }
    void update(const void* data, std::size_t size) {
        if (EVP_DigestUpdate(context_, data, size) != 1)
            throw std::runtime_error("SHA-256 update failed");
    }
    void text(const std::string& value) { update(value.data(), value.size()); }
    std::string finish() {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int size = 0;
        if (EVP_DigestFinal_ex(context_, digest, &size) != 1)
            throw std::runtime_error("SHA-256 finalization failed");
        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (unsigned int i = 0; i < size; ++i)
            output << std::setw(2) << static_cast<unsigned int>(digest[i]);
        return output.str();
    }
private:
    EVP_MD_CTX* context_ = nullptr;
};
#endif

std::optional<std::string> package_digest(const fs::path& package,
                                          const std::vector<fs::path>& files,
                                          std::string* error) {
#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
    try {
        Sha256 digest;
        for (const auto& relative : files) {
            const std::string name = relative.generic_string();
            const std::string name_size = std::to_string(name.size()) + "\n";
            const std::string file_size = std::to_string(fs::file_size(package / relative)) + "\n";
            digest.text(name_size);
            digest.text(name);
            digest.text("\n");
            digest.text(file_size);
            std::ifstream input(package / relative, std::ios::binary);
            char buffer[16384];
            while (input) {
                input.read(buffer, sizeof(buffer));
                if (input.gcount() > 0)
                    digest.update(buffer, static_cast<std::size_t>(input.gcount()));
            }
            if (!input.eof()) throw std::runtime_error("package file read failed");
        }
        return digest.finish();
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return std::nullopt;
    }
#else
    (void)package;
    (void)files;
    if (error) *error = "SHA-256 requires an OpenSSL-enabled build";
    return std::nullopt;
#endif
}

bool atomic_write(const fs::path& path, const std::string& content, std::string* error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        if (error) *error = "cannot create state directory: " + ec.message();
        return false;
    }
    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            if (error) *error = "cannot open temporary state file";
            return false;
        }
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.flush();
        if (!output) {
            if (error) *error = "cannot write temporary state file";
            return false;
        }
    }
    fs::rename(temporary, path, ec);
    if (ec) {
        std::error_code cleanup_error;
        fs::remove(temporary, cleanup_error);
        if (error) *error = "cannot publish state file: " + ec.message();
        return false;
    }
    return true;
}

SkillIndexEntry package_entry(const SkillPackageRecord& package) {
    SkillIndexEntry entry;
    entry.id = package.id;
    entry.name = package.manifest->name;
    entry.yaml_id = package.manifest->legacy_id;
    entry.description = package.manifest->description;
    entry.version = package.manifest->version;
    entry.license = package.manifest->license;
    entry.trigger_keywords = package.manifest->trigger_keywords;
    entry.tags = package.manifest->tags;
    entry.disable_model_invocation = package.manifest->disable_model_invocation;
    entry.allowed_tools = package.manifest->permissions.tools;
    entry.file_path = package.package_path / "SKILL.md";
    entry.script_jail = package.package_path;
    entry.manifest = package.manifest;
    entry.package_digest = package.package_digest;
    entry.resource_digests = package.resource_digests;
    entry.source_uri = package.source_uri;
    entry.signature_identity = package.signature_identity;
    entry.package_lease = package.lease;
    for (const auto& resource : package.manifest->resources) {
        if (resource.kind == SkillResourceType::Script) entry.scripts.push_back(resource.path);
        if (resource.kind == SkillResourceType::Reference) entry.references.push_back(resource.path);
        if (resource.kind == SkillResourceType::Cli) entry.cli_programs.push_back(resource.path);
    }
    return entry;
}

} // namespace

std::optional<SkillSemVersion> SkillSemVersion::parse(const std::string& raw,
                                                      std::string* error) {
    const std::string value = trim(raw);
    if (value.empty() || value != raw) {
        if (error) *error = "version must be non-empty and contain no surrounding whitespace";
        return std::nullopt;
    }
    SkillSemVersion version;
    const auto plus = value.find('+');
    if (plus != std::string::npos && value.find('+', plus + 1) != std::string::npos) {
        if (error) *error = "version contains multiple build separators";
        return std::nullopt;
    }
    const std::string without_build = value.substr(0, plus);
    if (plus != std::string::npos) {
        version.build = value.substr(plus + 1);
        if (!valid_semver_identifiers(version.build)) {
            if (error) *error = "invalid build metadata";
            return std::nullopt;
        }
    }
    const auto dash = without_build.find('-');
    const std::string core = without_build.substr(0, dash);
    if (dash != std::string::npos) {
        const std::string prerelease = without_build.substr(dash + 1);
        version.prerelease = split(prerelease, '.');
        if (!valid_semver_identifiers(prerelease)) {
            if (error) *error = "prerelease is empty";
            return std::nullopt;
        }
        for (const auto& identifier : version.prerelease) {
            if (!semver_identifier(identifier) ||
                (numeric_identifier(identifier) && identifier.size() > 1 && identifier[0] == '0')) {
                if (error) *error = "invalid prerelease identifier";
                return std::nullopt;
            }
        }
    }
    const auto parts = split(core, '.');
    if (parts.size() != 3 || !parse_number(parts[0], version.major) ||
        !parse_number(parts[1], version.minor) || !parse_number(parts[2], version.patch)) {
        if (error) *error = "version must contain major.minor.patch";
        return std::nullopt;
    }
    return version;
}

std::string SkillSemVersion::str() const {
    std::string output = std::to_string(major) + "." + std::to_string(minor) + "." +
                         std::to_string(patch);
    if (!prerelease.empty()) {
        output += "-";
        for (std::size_t i = 0; i < prerelease.size(); ++i) {
            if (i) output += ".";
            output += prerelease[i];
        }
    }
    if (!build.empty()) output += "+" + build;
    return output;
}

bool operator<(const SkillSemVersion& lhs, const SkillSemVersion& rhs) {
    if (lhs.major != rhs.major) return lhs.major < rhs.major;
    if (lhs.minor != rhs.minor) return lhs.minor < rhs.minor;
    if (lhs.patch != rhs.patch) return lhs.patch < rhs.patch;
    if (lhs.prerelease.empty() != rhs.prerelease.empty()) return !lhs.prerelease.empty();
    for (std::size_t i = 0; i < std::min(lhs.prerelease.size(), rhs.prerelease.size()); ++i) {
        const int compared = compare_identifiers(lhs.prerelease[i], rhs.prerelease[i]);
        if (compared != 0) return compared < 0;
    }
    return lhs.prerelease.size() < rhs.prerelease.size();
}

std::optional<SkillSemVersionRange> SkillSemVersionRange::parse(
    const std::string& raw, std::string* error) {
    SkillSemVersionRange range;
    range.expression_ = trim(raw);
    if (range.expression_.empty()) range.expression_ = "*";
    std::size_t start = 0;
    while (start <= range.expression_.size()) {
        const auto separator = range.expression_.find("||", start);
        std::string alternative = trim(range.expression_.substr(
            start, separator == std::string::npos ? std::string::npos : separator - start));
        if (alternative.empty()) {
            if (error) *error = "version range contains an empty alternative";
            return std::nullopt;
        }
        std::replace(alternative.begin(), alternative.end(), ',', ' ');
        std::istringstream tokens(alternative);
        std::string token;
        std::vector<Comparator> comparators;
        while (tokens >> token) {
            if (token == "*" || token == "x" || token == "X") continue;
            std::string op;
            if (token.rfind(">=", 0) == 0 || token.rfind("<=", 0) == 0) {
                op = token.substr(0, 2); token.erase(0, 2);
            } else if (!token.empty() && std::string("=<>^~").find(token[0]) != std::string::npos) {
                op = token.substr(0, 1); token.erase(0, 1);
            }
            auto parts = split(token, '.');
            const bool wildcard = parts.size() < 3 ||
                std::any_of(parts.begin(), parts.end(), [](const std::string& part) {
                    return part == "*" || part == "x" || part == "X";
                });
            if (wildcard) {
                if (!op.empty()) {
                    if (error) *error = "comparators require a complete semantic version";
                    return std::nullopt;
                }
                std::uint64_t major = 0, minor = 0;
                if (parts.empty() || parts[0] == "*" || parts[0] == "x" || parts[0] == "X") continue;
                if (!parse_number(parts[0], major) ||
                    (parts.size() > 1 && parts[1] != "*" && parts[1] != "x" &&
                     parts[1] != "X" && !parse_number(parts[1], minor))) {
                    if (error) *error = "invalid wildcard range";
                    return std::nullopt;
                }
                SkillSemVersion lower{major, minor, 0};
                SkillSemVersion upper = parts.size() <= 1 || parts[1] == "*" ||
                    parts[1] == "x" || parts[1] == "X"
                    ? SkillSemVersion{major + 1, 0, 0}
                    : SkillSemVersion{major, minor + 1, 0};
                comparators.push_back({Comparator::Op::Gte, lower});
                comparators.push_back({Comparator::Op::Lt, upper});
                continue;
            }
            auto version = SkillSemVersion::parse(token, error);
            if (!version) return std::nullopt;
            if (op == "^") {
                comparators.push_back({Comparator::Op::Gte, *version});
                SkillSemVersion upper = version->major > 0
                    ? SkillSemVersion{version->major + 1, 0, 0}
                    : version->minor > 0 ? SkillSemVersion{0, version->minor + 1, 0}
                                         : SkillSemVersion{0, 0, version->patch + 1};
                comparators.push_back({Comparator::Op::Lt, upper});
            } else if (op == "~") {
                comparators.push_back({Comparator::Op::Gte, *version});
                comparators.push_back({Comparator::Op::Lt,
                                       SkillSemVersion{version->major, version->minor + 1, 0}});
            } else {
                Comparator::Op operation = Comparator::Op::Eq;
                if (op == ">") operation = Comparator::Op::Gt;
                else if (op == ">=") operation = Comparator::Op::Gte;
                else if (op == "<") operation = Comparator::Op::Lt;
                else if (op == "<=") operation = Comparator::Op::Lte;
                comparators.push_back({operation, *version});
            }
        }
        range.alternatives_.push_back(std::move(comparators));
        if (separator == std::string::npos) break;
        start = separator + 2;
    }
    return range;
}

bool SkillSemVersionRange::contains(const SkillSemVersion& version) const {
    for (const auto& alternative : alternatives_) {
        bool accepted = true;
        bool prerelease_anchor = version.prerelease.empty();
        for (const auto& comparator : alternative) {
            const bool less = version < comparator.version;
            const bool greater = comparator.version < version;
            const bool equal = !less && !greater;
            if (!version.prerelease.empty() && !comparator.version.prerelease.empty() &&
                version.major == comparator.version.major && version.minor == comparator.version.minor &&
                version.patch == comparator.version.patch) prerelease_anchor = true;
            switch (comparator.op) {
            case Comparator::Op::Eq: accepted = accepted && equal; break;
            case Comparator::Op::Lt: accepted = accepted && less; break;
            case Comparator::Op::Lte: accepted = accepted && (less || equal); break;
            case Comparator::Op::Gt: accepted = accepted && greater; break;
            case Comparator::Op::Gte: accepted = accepted && (greater || equal); break;
            }
        }
        if (accepted && prerelease_anchor) return true;
    }
    return false;
}

SkillDependencyResolution SkillDependencyResolver::resolve(
    const std::vector<SkillDependencyRequirement>& roots) const {
    SkillDependencyResolution result;
    std::vector<SkillDependencyRequirement> requirements = roots;
    std::map<std::string, SkillPackageRecord> selected;
    std::set<std::string> expanding;
    std::vector<SkillDependencyRequirement> last_conflict;
    std::function<bool()> search = [&]() -> bool {
        std::string unresolved;
        for (const auto& requirement : requirements) {
            const auto chosen = selected.find(requirement.skill_id);
            std::string parse_error;
            auto range = SkillSemVersionRange::parse(requirement.range, &parse_error);
            if (!range) { last_conflict = {requirement}; return false; }
            if (chosen != selected.end() && !range->contains(chosen->second.version)) {
                last_conflict.clear();
                for (const auto& item : requirements)
                    if (item.skill_id == requirement.skill_id) last_conflict.push_back(item);
                return false;
            }
            if (chosen == selected.end() &&
                (!requirement.optional || catalog_.contains(requirement.skill_id))) {
                unresolved = requirement.skill_id;
                break;
            }
        }
        if (unresolved.empty()) return true;
        const auto available = catalog_.find(unresolved);
        if (available == catalog_.end()) {
            for (const auto& item : requirements)
                if (item.skill_id == unresolved && !item.optional) last_conflict.push_back(item);
            return false;
        }
        std::vector<SkillPackageRecord> candidates = available->second;
        std::sort(candidates.begin(), candidates.end(), [](const auto& lhs, const auto& rhs) {
            if (!(lhs.version < rhs.version) && !(rhs.version < lhs.version))
                return lhs.package_digest < rhs.package_digest;
            return rhs.version < lhs.version;
        });
        for (const auto& candidate : candidates) {
            bool compatible = true;
            for (const auto& requirement : requirements) {
                if (requirement.skill_id != unresolved) continue;
                auto range = SkillSemVersionRange::parse(requirement.range);
                compatible = compatible && range && range->contains(candidate.version);
            }
            if (!compatible) continue;
            selected[unresolved] = candidate;
            const std::size_t previous_size = requirements.size();
            expanding.insert(unresolved);
            std::vector<std::string> parent_path{unresolved};
            for (const auto& requirement : requirements)
                if (requirement.skill_id == unresolved && !requirement.path.empty() &&
                    (parent_path.size() == 1 || requirement.path.size() < parent_path.size()))
                    parent_path = requirement.path;
            bool cycle = false;
            for (const auto& dependency : candidate.manifest->dependencies) {
                if (std::find(parent_path.begin(), parent_path.end(), dependency.name) !=
                    parent_path.end()) {
                    auto cycle_path = parent_path;
                    cycle_path.push_back(dependency.name);
                    last_conflict = {{unresolved, dependency.name, dependency.version,
                                      dependency.optional, std::move(cycle_path)}};
                    cycle = true;
                    break;
                }
                SkillDependencyRequirement requirement;
                requirement.requester = unresolved;
                requirement.skill_id = dependency.name;
                requirement.range = dependency.version;
                requirement.optional = dependency.optional;
                requirement.path = parent_path;
                requirement.path.push_back(dependency.name);
                requirements.push_back(std::move(requirement));
            }
            expanding.erase(unresolved);
            if (cycle) {
                requirements.resize(previous_size);
                selected.erase(unresolved);
                continue;
            }
            if (search()) return true;
            requirements.resize(previous_size);
            selected.erase(unresolved);
        }
        if (last_conflict.empty()) {
            for (const auto& item : requirements)
                if (item.skill_id == unresolved) last_conflict.push_back(item);
        }
        const auto versions = catalog_.find(unresolved);
        if (versions != catalog_.end() && last_conflict.size() > 1) {
            auto has_candidate = [&](const std::vector<SkillDependencyRequirement>& constraints) {
                for (const auto& candidate : versions->second) {
                    bool accepted = true;
                    for (const auto& constraint : constraints) {
                        auto range = SkillSemVersionRange::parse(constraint.range);
                        accepted = accepted && range && range->contains(candidate.version);
                    }
                    if (accepted) return true;
                }
                return false;
            };
            for (std::size_t i = 0; i < last_conflict.size();) {
                auto reduced = last_conflict;
                reduced.erase(reduced.begin() + static_cast<std::ptrdiff_t>(i));
                if (!reduced.empty() && !has_candidate(reduced)) last_conflict = std::move(reduced);
                else ++i;
            }
        }
        return false;
    };
    if (search()) {
        result.ok = true;
        result.packages = std::move(selected);
        return result;
    }
    result.conflict = std::move(last_conflict);
    json constraints = json::array();
    for (const auto& conflict : result.conflict)
        constraints.push_back({{"requester", conflict.requester}, {"skill", conflict.skill_id},
                               {"range", conflict.range}, {"path", conflict.path}});
    result.error = failure(kSkillDependencyConflict, "Skill dependency graph cannot be resolved",
                           {{"conflict", constraints},
                            {"suggestion", "install a compatible version or relax one conflicting range"}});
    return result;
}

json SkillLockfile::to_json() const {
    auto sorted_roots = roots;
    auto sorted_packages = packages;
    auto sorted_edges = edges;
    std::sort(sorted_roots.begin(), sorted_roots.end());
    std::sort(sorted_packages.begin(), sorted_packages.end(), [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.id, lhs.version, lhs.package_digest) <
               std::tie(rhs.id, rhs.version, rhs.package_digest);
    });
    std::sort(sorted_edges.begin(), sorted_edges.end(), [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.from, lhs.to, lhs.range, lhs.optional) <
               std::tie(rhs.from, rhs.to, rhs.range, rhs.optional);
    });
    json output = {{"apiVersion", "agent.taskflow/skills-lock/v1"},
                   {"roots", sorted_roots}, {"rootRanges", root_ranges},
                   {"packages", json::array()}, {"edges", json::array()}};
    for (const auto& package : sorted_packages)
        output["packages"].push_back({{"id", package.id}, {"version", package.version},
            {"packageDigest", package.package_digest}, {"resourceDigests", package.resource_digests},
            {"sourceUri", package.source_uri}, {"signatureIdentity", package.signature_identity},
            {"archiveDigest", package.archive_digest}, {"publisher", package.publisher},
            {"keyId", package.key_id}, {"signatureDigest", package.signature_digest},
            {"sbomDigest", package.sbom_digest}, {"provenanceDigest", package.provenance_digest},
            {"registryDigest", package.registry_digest}, {"legacyUnsigned", package.legacy_unsigned}});
    for (const auto& edge : sorted_edges)
        output["edges"].push_back({{"from", edge.from}, {"to", edge.to},
                                   {"range", edge.range}, {"optional", edge.optional}});
    return output;
}

std::optional<SkillLockfile> SkillLockfile::from_json(const json& value, std::string* error) {
    try {
        if (!value.is_object() || value.value("apiVersion", "") != "agent.taskflow/skills-lock/v1")
            throw std::runtime_error("unsupported lockfile API version");
        SkillLockfile lock;
        lock.roots = value.at("roots").get<std::vector<std::string>>();
        lock.root_ranges = value.value(
            "rootRanges", std::map<std::string, std::string>{});
        std::set<std::string> package_ids;
        for (const auto& item : value.at("packages")) {
            SkillLockPackage package;
            package.id = item.at("id").get<std::string>();
            package.version = item.at("version").get<std::string>();
            package.package_digest = item.at("packageDigest").get<std::string>();
            package.resource_digests = item.value("resourceDigests", std::map<std::string, std::string>{});
            package.source_uri = item.value("sourceUri", "");
            package.signature_identity = item.value("signatureIdentity", "");
            package.archive_digest = item.value("archiveDigest", "");
            package.publisher = item.value("publisher", "");
            package.key_id = item.value("keyId", "");
            package.signature_digest = item.value("signatureDigest", "");
            package.sbom_digest = item.value("sbomDigest", "");
            package.provenance_digest = item.value("provenanceDigest", "");
            package.registry_digest = item.value("registryDigest", "");
            package.legacy_unsigned = item.value("legacyUnsigned", package.key_id.empty());
            if (package.id.empty() || !package_ids.insert(package.id).second ||
                !SkillSemVersion::parse(package.version) || !sha256_identity(package.package_digest))
                throw std::runtime_error("lockfile package identity is invalid");
            for (const auto& [resource, digest] : package.resource_digests)
                if (resource.empty() || !sha256_identity(digest))
                    throw std::runtime_error("lockfile resource digest is invalid");
            for(const auto* digest : {&package.archive_digest, &package.signature_digest,
                                      &package.sbom_digest, &package.provenance_digest,
                                      &package.registry_digest})
                if(!digest->empty() && !sha256_identity(*digest))
                    throw std::runtime_error("lockfile supply-chain digest is invalid");
            if(!package.legacy_unsigned && (package.archive_digest.empty() || package.publisher.empty() ||
               package.key_id.empty() || package.signature_digest.empty() ||
               package.sbom_digest.empty() || package.provenance_digest.empty()))
                throw std::runtime_error("verified lockfile identity is incomplete");
            lock.packages.push_back(std::move(package));
        }
        std::set<std::string> roots;
        for (const auto& root : lock.roots)
            if (root.empty() || !roots.insert(root).second || !package_ids.contains(root))
                throw std::runtime_error("lockfile root is invalid");
        for (const auto& [root, range] : lock.root_ranges)
            if (!roots.contains(root) || !SkillSemVersionRange::parse(range))
                throw std::runtime_error("lockfile root range is invalid");
        for (const auto& item : value.at("edges")) {
            SkillLockEdge edge{item.at("from").get<std::string>(),
                               item.at("to").get<std::string>(),
                               item.at("range").get<std::string>(),
                               item.value("optional", false)};
            if (!package_ids.contains(edge.from) || edge.to.empty() ||
                (!edge.optional && !package_ids.contains(edge.to)) ||
                !SkillSemVersionRange::parse(edge.range))
                throw std::runtime_error("lockfile dependency edge is invalid");
            lock.edges.push_back(std::move(edge));
        }
        return lock;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return std::nullopt;
    }
}

std::string SkillLockfile::dump() const { return to_json().dump(2) + "\n"; }

bool SkillLockfile::save(const fs::path& path, std::string* error) const {
    return atomic_write(path, dump(), error);
}

std::optional<SkillLockfile> SkillLockfile::load(const fs::path& path, std::string* error) {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("lockfile is unavailable");
        json value;
        input >> value;
        return from_json(value, error);
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return std::nullopt;
    }
}

std::optional<std::string> skill_sha256_file(const fs::path& path, std::string* error) {
#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
    try {
        Sha256 digest;
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("file is unavailable");
        char buffer[16384];
        while (input) {
            input.read(buffer, sizeof(buffer));
            if (input.gcount() > 0) digest.update(buffer, static_cast<std::size_t>(input.gcount()));
        }
        if (!input.eof()) throw std::runtime_error("file read failed");
        return digest.finish();
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return std::nullopt;
    }
#else
    (void)path;
    if (error) *error = "SHA-256 requires an OpenSSL-enabled build";
    return std::nullopt;
#endif
}

SkillLifecycleResult inspect_skill_package(const fs::path& package) {
    std::string error;
    auto manifest = read_manifest(package, &error);
    if (!manifest) {
        const auto code = error.starts_with("resource_hash_mismatch:")
            ? kSkillDigestMismatch : kSkillLifecycleInvalid;
        return {false, failure(code, error)};
    }
    auto version = SkillSemVersion::parse(manifest->version, &error);
    if (!version) return {false, failure(kSkillLifecycleInvalid, error)};
    const auto files = package_files(package, &error);
    if (files.empty() || std::find(files.begin(), files.end(), fs::path("SKILL.md")) == files.end())
        return {false, failure(kSkillLifecycleInvalid,
                               error.empty() ? "package has no files" : error)};
    auto digest = package_digest(package, files, &error);
    if (!digest) return {false, failure(kSkillLifecycleInvalid, error)};
    std::map<std::string, std::string> resource_digests;
    for (const auto& resource : manifest->resources) {
        auto resource_digest = skill_sha256_file(package / resource.path, &error);
        if (!resource_digest && !resource.optional)
            return {false, failure(kSkillLifecycleInvalid, error,
                                   {{"resource", resource.id}})};
        if (!resource_digest) continue;
        if (!resource.sha256.empty() && resource.sha256 != *resource_digest)
            return {false, failure(kSkillDigestMismatch,
                                   "declared resource digest does not match package content",
                                   {{"resource", resource.id},
                                    {"expected", resource.sha256},
                                    {"actual", *resource_digest}})};
        resource_digests[resource.id] = *resource_digest;
    }
    SkillPackageRecord record;
    record.id = !manifest->name.empty() ? manifest->name : manifest->legacy_id;
    record.version = *version;
    record.package_digest = *digest;
    record.resource_digests = std::move(resource_digests);
    record.package_path = package;
    record.manifest = std::make_shared<SkillManifest>(std::move(*manifest));
    return {true, json::object(), std::move(record), std::nullopt};
}

SkillPackageStore::SkillPackageStore(fs::path root) : root_(std::move(root)) {
    std::error_code ec;
    fs::create_directories(root_ / "sha256", ec);
    fs::create_directories(root_ / "transactions", ec);
}

SkillLifecycleResult SkillPackageStore::import_package(
    const fs::path& package, const SkillInstallOptions& options) {
    std::string error;
    auto inspected = inspect_skill_package(package);
    if (!inspected.ok || !inspected.package) return inspected;
    const auto& identity = *inspected.package;
    const auto files = package_files(package, &error);
    if (files.empty()) return {false, failure(kSkillLifecycleInvalid, error)};
    const fs::path destination = root_ / "sha256" / identity.package_digest / "package";
    std::error_code ec;
    if (!fs::exists(destination)) {
        const fs::path temporary = root_ / "transactions" / (identity.package_digest + ".tmp");
        fs::remove_all(temporary, ec);
        fs::create_directories(temporary, ec);
        if (ec) return {false, failure(kSkillLifecycleInvalid, "cannot create transaction directory")};
        for (const auto& relative : files) {
            fs::create_directories((temporary / "package" / relative).parent_path(), ec);
            fs::copy_file(package / relative, temporary / "package" / relative,
                          fs::copy_options::overwrite_existing, ec);
            if (ec) {
                fs::remove_all(temporary, ec);
                return {false, failure(kSkillLifecycleInvalid, "package copy failed")};
            }
        }
        auto copied_files = package_files(temporary / "package", &error);
        auto copied_digest = package_digest(temporary / "package", copied_files, &error);
        if (!copied_digest || *copied_digest != identity.package_digest) {
            fs::remove_all(temporary, ec);
            return {false, failure(kSkillDigestMismatch, "copied package digest changed")};
        }
        if(!options.archive_path.empty()) {
            auto archive = inspect_skill_archive(options.archive_path);
            if(!archive.ok || archive.archive_digest != options.archive_digest) {
                fs::remove_all(temporary, ec);
                return {false, failure(kSkillDigestMismatch, "source archive identity changed")};
            }
            fs::copy_file(options.archive_path, temporary / "archive.tfskill",
                          fs::copy_options::overwrite_existing, ec);
            if(ec) {
                fs::remove_all(temporary, ec);
                return {false, failure(kSkillLifecycleInvalid, "archive copy failed")};
            }
        }
        json metadata = {{"id", identity.id}, {"version", identity.version.str()},
                         {"packageDigest", identity.package_digest},
                         {"resourceDigests", identity.resource_digests},
                         {"sourceUri", options.source_uri},
                         {"signatureIdentity", options.signature_identity},
                         {"archiveDigest", options.archive_digest},
                         {"publisher", options.publisher}, {"keyId", options.key_id},
                         {"signatureDigest", options.signature_digest},
                         {"sbomDigest", options.sbom_digest},
                         {"provenanceDigest", options.provenance_digest},
                         {"registryDigest", options.registry_digest},
                         {"legacyUnsigned", options.legacy_unsigned},
                         {"signatureEnvelope", options.signature
                            ? options.signature->to_json() : json(nullptr)}};
        if (!atomic_write(temporary / "metadata.json", metadata.dump(2) + "\n", &error)) {
            fs::remove_all(temporary, ec);
            return {false, failure(kSkillLifecycleInvalid, error)};
        }
        fs::create_directories(destination.parent_path().parent_path(), ec);
        fs::rename(temporary, destination.parent_path(), ec);
        if (ec && !fs::exists(destination)) {
            fs::remove_all(temporary, ec);
            return {false, failure(kSkillLifecycleInvalid, "package publish failed")};
        }
    }
    auto loaded = load(identity.package_digest, &error);
    if(loaded && !options.archive_digest.empty() &&
       (loaded->archive_digest != options.archive_digest || loaded->key_id != options.key_id ||
        loaded->signature_digest != options.signature_digest))
        return {false, failure(kSkillDigestMismatch,
                               "existing store identity differs from verified import")};
    return loaded ? SkillLifecycleResult{true, json::object(), loaded, std::nullopt}
                  : SkillLifecycleResult{false, failure(kSkillLifecycleInvalid, error)};
}

std::optional<SkillPackageRecord> SkillPackageStore::load(
    const std::string& digest, std::string* error) const {
    try {
        if (!sha256_identity(digest)) throw std::runtime_error("package digest is invalid");
        const fs::path directory = root_ / "sha256" / digest;
        std::ifstream metadata_input(directory / "metadata.json", std::ios::binary);
        if (!metadata_input) throw std::runtime_error("package metadata is unavailable");
        json metadata;
        metadata_input >> metadata;
        const fs::path package = directory / "package";
        const auto files = package_files(package, error);
        auto actual = package_digest(package, files, error);
        if (!actual || *actual != digest || metadata.value("packageDigest", "") != digest)
            throw std::runtime_error("package digest does not match store identity");
        auto manifest = read_manifest(package, error);
        if (!manifest) return std::nullopt;
        auto version = SkillSemVersion::parse(manifest->version, error);
        if (!version) return std::nullopt;
        const std::string id = !manifest->name.empty() ? manifest->name : manifest->legacy_id;
        if (metadata.value("id", "") != id || metadata.value("version", "") != manifest->version)
            throw std::runtime_error("package metadata does not match its manifest");
        const auto archive_digest = metadata.value("archiveDigest", "");
        const auto signature_digest = metadata.value("signatureDigest", "");
        const auto sbom_digest = metadata.value("sbomDigest", "");
        const auto provenance_digest = metadata.value("provenanceDigest", "");
        const auto registry_digest = metadata.value("registryDigest", "");
        const bool legacy_unsigned = metadata.value("legacyUnsigned", true);
        for(const auto* supply_digest : {&archive_digest, &signature_digest, &sbom_digest,
                                         &provenance_digest, &registry_digest})
            if(!supply_digest->empty() && !sha256_identity(*supply_digest))
                throw std::runtime_error("stored supply-chain digest is invalid");
        SkillArchiveResult stored_archive;
        if(!archive_digest.empty()) {
            stored_archive = inspect_skill_archive(directory / "archive.tfskill");
            if(!stored_archive.ok || stored_archive.archive_digest != archive_digest)
                throw std::runtime_error("stored archive digest does not match metadata");
        }
        if(!legacy_unsigned) {
            if(!metadata.contains("signatureEnvelope") || !metadata.at("signatureEnvelope").is_object())
                throw std::runtime_error("verified package signature envelope is unavailable");
            std::string signature_error;
            auto envelope = SkillSignatureEnvelope::from_json(metadata.at("signatureEnvelope"),
                                                               &signature_error);
            auto actual_signature_digest = skill_sha256_bytes(
                metadata.at("signatureEnvelope").dump(), &signature_error);
            if(!envelope || !actual_signature_digest || *actual_signature_digest != signature_digest ||
               envelope->subject_digest != archive_digest || envelope->sbom_digest != sbom_digest ||
               envelope->provenance_digest != provenance_digest ||
               envelope->key_id != metadata.value("keyId", "") ||
               envelope->publisher != metadata.value("publisher", ""))
                throw std::runtime_error("stored signature identity does not match metadata");
            SkillPackageMetadata expected;
            expected.archive_digest = archive_digest;
            expected.sbom_digest = sbom_digest;
            expected.provenance_digest = provenance_digest;
            expected.entry_count = stored_archive.entries.size();
            auto audited = inspect_audited_skill_archive(directory / "archive.tfskill", expected);
            if(!audited.ok) throw std::runtime_error("stored archive audit metadata is invalid");
        }
        const auto recorded_resource_digests = metadata.value(
            "resourceDigests", std::map<std::string, std::string>{});
        std::map<std::string, std::string> actual_resource_digests;
        for (const auto& resource : manifest->resources) {
            std::error_code resource_error;
            if (!fs::is_regular_file(package / resource.path, resource_error)) {
                if (resource.optional) continue;
                throw std::runtime_error("required package resource is unavailable");
            }
            auto resource_digest = skill_sha256_file(package / resource.path, error);
            if (!resource_digest) return std::nullopt;
            actual_resource_digests[resource.id] = *resource_digest;
        }
        if (recorded_resource_digests != actual_resource_digests)
            throw std::runtime_error("package resource digests do not match metadata");
        std::shared_ptr<const void> lease;
        {
            std::lock_guard<std::mutex> lock(lease_mutex_);
            lease = leases_[digest].lock();
            if (!lease) {
                lease = std::make_shared<const std::string>(digest);
                leases_[digest] = lease;
            }
        }
        SkillPackageRecord record;
        record.id = id;
        record.version = *version;
        record.package_digest = digest;
        record.resource_digests = std::move(actual_resource_digests);
        record.source_uri = metadata.value("sourceUri", "");
        record.signature_identity = metadata.value("signatureIdentity", "");
        record.archive_digest = archive_digest;
        record.publisher = metadata.value("publisher", "");
        record.key_id = metadata.value("keyId", "");
        record.signature_digest = signature_digest;
        record.sbom_digest = sbom_digest;
        record.provenance_digest = provenance_digest;
        record.registry_digest = registry_digest;
        record.legacy_unsigned = legacy_unsigned;
        record.package_path = package;
        record.manifest = std::make_shared<SkillManifest>(std::move(*manifest));
        record.lease = std::move(lease);
        return record;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return std::nullopt;
    }
}

std::vector<SkillPackageRecord> SkillPackageStore::catalog(std::string* error) const {
    std::vector<SkillPackageRecord> output;
    std::error_code ec;
    const fs::path base = root_ / "sha256";
    for (const auto& item : fs::directory_iterator(base, ec)) {
        if (ec) break;
        if (!item.is_directory()) continue;
        auto package = load(item.path().filename().string(), error);
        if (!package) return {};
        output.push_back(std::move(*package));
    }
    std::sort(output.begin(), output.end(), [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.id, lhs.version, lhs.package_digest) <
               std::tie(rhs.id, rhs.version, rhs.package_digest);
    });
    return output;
}

bool SkillPackageStore::remove(const std::string& digest, std::string* error) {
    if (!sha256_identity(digest)) {
        if (error) *error = "package digest is invalid";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(lease_mutex_);
        const auto found = leases_.find(digest);
        if (found != leases_.end() && !found->second.expired()) {
            if (error) *error = "package is referenced by an active snapshot";
            return false;
        }
    }
    std::error_code ec;
    const auto count = fs::remove_all(root_ / "sha256" / digest, ec);
    if (ec || count == 0) {
        if (error) *error = ec ? ec.message() : "package is unavailable";
        return false;
    }
    return true;
}

SkillLifecycleManager::SkillLifecycleManager(
    std::shared_ptr<SkillRegistry> registry, fs::path store_root,
    SkillAuditSink audit_sink)
    : registry_(std::move(registry)), store_(std::move(store_root)),
      state_directory_(store_.root() / "state"), audit_sink_(std::move(audit_sink)) {
    if (!registry_) throw std::invalid_argument("SkillLifecycleManager requires a Registry");
    std::string error;
    if (!recover(&error)) throw std::runtime_error(error);
}

SkillLifecycleResult SkillLifecycleManager::audited(
    std::string action, std::string target, SkillLifecycleResult result) const {
    SkillAuditIdentity identity;
    identity.skill_id = target;
    identity.registry_generation = registry_->snapshot().generation();
    const auto code = result.ok || !result.error.is_object()
        ? std::string{} : result.error.value("code", "");
    emit_skill_audit(audit_sink_,
        {std::move(identity), "lifecycle", std::move(action), {},
         result.ok ? "completed" : "failed", code, {{"target", std::move(target)}}});
    return result;
}

bool SkillLifecycleManager::recover(std::string* error) {
    std::error_code ec;
    fs::create_directories(state_directory_, ec);
    for (const auto& item : fs::directory_iterator(store_.root() / "transactions", ec))
        fs::remove_all(item.path(), ec);
    if (fs::exists(state_directory_ / "history.json")) {
        try {
            std::ifstream input(state_directory_ / "history.json");
            json values;
            input >> values;
            for (const auto& value : values) {
                auto lock = SkillLockfile::from_json(value, error);
                if (!lock) return false;
                history_.push_back(std::move(*lock));
            }
        } catch (const std::exception& exception) {
            if (error) *error = exception.what();
            return false;
        }
    }
    if (fs::exists(state_directory_ / "skills.lock")) {
        auto lock = SkillLockfile::load(state_directory_ / "skills.lock", error);
        if (!lock) return false;
        active_lock_ = std::move(*lock);
        for (const auto& root : active_lock_.roots) requested_roots_[root] = [&] {
            const auto requested = active_lock_.root_ranges.find(root);
            if (requested != active_lock_.root_ranges.end()) return requested->second;
            for (const auto& package : active_lock_.packages)
                if (package.id == root) return "=" + package.version;
            return std::string("=0.0.0");
        }();
        auto result = publish_lock(active_lock_);
        if (!result.ok) {
            if (error) *error = result.error.dump();
            return false;
        }
    }
    return true;
}

SkillLifecycleResult SkillLifecycleManager::install(
    const fs::path& package, const SkillInstallOptions& options) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(fs::is_directory(package))
        return audited("install", package.filename().string(), store_.import_package(package, options));
    SkillPackageGate::TrustOptions trust_options;
    trust_options.trust = options.trust;
    trust_options.signature = options.signature;
    trust_options.remote = options.remote;
    trust_options.allow_unsigned_local = options.allow_unsigned_local;
    trust_options.now = options.verification_time;
    auto admitted = SkillPackageGate{}.inspect_archive(package, trust_options);
    if(!admitted.ok) return {false, admitted.error};
    const auto temporary = store_.root() / "transactions" /
        ("admit-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto extracted = extract_skill_archive(package, temporary);
    if(!extracted.ok) return {false, failure(kSkillArchiveInvalid, extracted.error)};
    auto verified_options = options;
    verified_options.source_uri = options.signature ? options.signature->source_uri : "local://unsigned";
    verified_options.signature_identity = options.signature ? options.signature->key_id : "legacyUnsigned";
    verified_options.archive_path = package;
    verified_options.archive_digest = extracted.archive_digest;
    verified_options.legacy_unsigned = !options.signature.has_value();
    if(options.signature) {
        verified_options.publisher = options.signature->publisher;
        verified_options.key_id = options.signature->key_id;
        verified_options.sbom_digest = options.signature->sbom_digest;
        verified_options.provenance_digest = options.signature->provenance_digest;
        auto digest = skill_sha256_bytes(options.signature->to_json().dump());
        if(!digest) { fs::remove_all(temporary); return {false, failure(kSkillSignatureInvalid,
            "cannot digest verified signature envelope")}; }
        verified_options.signature_digest = *digest;
    }
    auto result = store_.import_package(temporary, verified_options);
    std::error_code ec;
    fs::remove_all(temporary, ec);
    return audited("install", package.filename().string(), std::move(result));
}

SkillLifecycleResult SkillLifecycleManager::enable(
    const std::string& skill_id, const std::string& range) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto previous = requested_roots_.find(skill_id);
    const std::optional<std::string> previous_range = previous == requested_roots_.end()
        ? std::nullopt : std::optional<std::string>(previous->second);
    requested_roots_[skill_id] = range;
    auto result = publish_resolved();
    if (!result.ok) {
        if (previous_range) requested_roots_[skill_id] = *previous_range;
        else requested_roots_.erase(skill_id);
    }
    return audited("enable", skill_id, std::move(result));
}

SkillLifecycleResult SkillLifecycleManager::disable(const std::string& skill_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = requested_roots_.find(skill_id);
    if (found == requested_roots_.end())
        return {false, failure(kSkillLifecycleInvalid, "Skill is not enabled")};
    const std::string previous = found->second;
    requested_roots_.erase(found);
    auto result = publish_resolved();
    if (!result.ok) requested_roots_[skill_id] = previous;
    return audited("disable", skill_id, std::move(result));
}

SkillLifecycleResult SkillLifecycleManager::update(
    const fs::path& package, const SkillInstallOptions& options) {
    std::lock_guard<std::mutex> lock(mutex_);
    SkillLifecycleResult installed;
    if(fs::is_directory(package)) {
        installed = store_.import_package(package, options);
    } else {
        SkillPackageGate::TrustOptions trust_options;
        trust_options.trust = options.trust;
        trust_options.signature = options.signature;
        trust_options.remote = options.remote;
        trust_options.allow_unsigned_local = options.allow_unsigned_local;
        trust_options.now = options.verification_time;
        auto admitted = SkillPackageGate{}.inspect_archive(package, trust_options);
        if(!admitted.ok) return {false, admitted.error};
        const auto temporary = store_.root() / "transactions" /
            ("admit-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        auto extracted = extract_skill_archive(package, temporary);
        if(!extracted.ok) return {false, failure(kSkillArchiveInvalid, extracted.error)};
        auto verified_options = options;
        verified_options.source_uri = options.signature ? options.signature->source_uri : "local://unsigned";
        verified_options.signature_identity = options.signature ? options.signature->key_id : "legacyUnsigned";
        verified_options.archive_path = package;
        verified_options.archive_digest = extracted.archive_digest;
        verified_options.legacy_unsigned = !options.signature.has_value();
        if(options.signature) {
            verified_options.publisher = options.signature->publisher;
            verified_options.key_id = options.signature->key_id;
            verified_options.sbom_digest = options.signature->sbom_digest;
            verified_options.provenance_digest = options.signature->provenance_digest;
            auto digest = skill_sha256_bytes(options.signature->to_json().dump());
            if(!digest) { fs::remove_all(temporary); return {false, failure(kSkillSignatureInvalid,
                "cannot digest verified signature envelope")}; }
            verified_options.signature_digest = *digest;
        }
        installed = store_.import_package(temporary, verified_options);
        std::error_code ec;
        fs::remove_all(temporary, ec);
    }
    if (!installed.ok || !installed.package) return installed;
    if (requested_roots_.empty()) return installed;
    auto result = publish_resolved();
    return audited("update", package.filename().string(), std::move(result));
}

SkillLifecycleResult SkillLifecycleManager::rollback(const std::string& skill_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
        const auto found = std::find_if(it->packages.begin(), it->packages.end(),
                                       [&](const auto& package) { return package.id == skill_id; });
        if (found == it->packages.end()) continue;
        const SkillLockfile target = *it;
        auto next_history = history_;
        const auto offset = static_cast<std::size_t>(std::distance(history_.rbegin(), it));
        next_history.erase(next_history.end() - 1 - static_cast<std::ptrdiff_t>(offset));
        json history = json::array();
        for (const auto& generation : next_history) history.push_back(generation.to_json());
        std::string error;
        if (!atomic_write(state_directory_ / "history.json", history.dump(2) + "\n", &error))
            return {false, failure(kSkillLockInvalid, error)};
        auto result = publish_lock(target);
        if (!result.ok) return result;
        history_ = std::move(next_history);
        requested_roots_.clear();
        for (const auto& root : target.roots) {
            const auto requested = target.root_ranges.find(root);
            if (requested != target.root_ranges.end()) {
                requested_roots_[root] = requested->second;
                continue;
            }
            const auto root_package = std::find_if(
                target.packages.begin(), target.packages.end(),
                [&](const auto& package) { return package.id == root; });
            if (root_package != target.packages.end())
                requested_roots_[root] = "=" + root_package->version;
        }
        return audited("rollback", skill_id, std::move(result));
    }
    return {false, failure(kSkillLifecycleInvalid, "No rollback generation is available")};
}

SkillLifecycleResult SkillLifecycleManager::remove(const std::string& digest) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& package : active_lock_.packages)
        if (package.package_digest == digest)
            return {false, failure(kSkillPackageInUse, "Package is selected by the active lockfile")};
    std::string error;
    auto result = store_.remove(digest, &error)
        ? SkillLifecycleResult{true, json::object()}
        : SkillLifecycleResult{false, failure(kSkillPackageInUse, error)};
    return audited("remove", digest, std::move(result));
}

SkillLifecycleResult SkillLifecycleManager::reload() {
    std::lock_guard<std::mutex> lock(mutex_);
    return audited("reload", {}, publish_resolved());
}

SkillLockfile SkillLifecycleManager::lockfile() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_lock_;
}

SkillLifecycleResult SkillLifecycleManager::publish_resolved() {
    std::string error;
    auto packages = store_.catalog(&error);
    if (!error.empty()) return {false, failure(kSkillLifecycleInvalid, error)};
    SkillDependencyResolver::Catalog catalog;
    for (auto& package : packages) catalog[package.id].push_back(std::move(package));
    std::vector<SkillDependencyRequirement> roots;
    for (const auto& [id, range] : requested_roots_)
        roots.push_back({"$root", id, range, false, {id}});
    auto resolution = SkillDependencyResolver(std::move(catalog)).resolve(roots);
    if (!resolution.ok) return {false, resolution.error};
    SkillLockfile next;
    for (const auto& [id, range] : requested_roots_) {
        next.roots.push_back(id);
        next.root_ranges[id] = range;
    }
    std::vector<SkillIndexEntry> entries;
    for (const auto& [id, package] : resolution.packages) {
        SkillLockPackage locked;
        locked.id = id;
        locked.version = package.version.str();
        locked.package_digest = package.package_digest;
        locked.resource_digests = package.resource_digests;
        locked.source_uri = package.source_uri;
        locked.signature_identity = package.signature_identity;
        locked.archive_digest = package.archive_digest;
        locked.publisher = package.publisher;
        locked.key_id = package.key_id;
        locked.signature_digest = package.signature_digest;
        locked.sbom_digest = package.sbom_digest;
        locked.provenance_digest = package.provenance_digest;
        locked.registry_digest = package.registry_digest;
        locked.legacy_unsigned = package.legacy_unsigned;
        next.packages.push_back(std::move(locked));
        entries.push_back(package_entry(package));
        for (const auto& dependency : package.manifest->dependencies)
            next.edges.push_back({id, dependency.name, dependency.version, dependency.optional});
    }
    auto next_history = history_;
    if (!active_lock_.packages.empty() && active_lock_.dump() != next.dump())
        next_history.push_back(active_lock_);
    json history = json::array();
    for (const auto& generation : next_history) history.push_back(generation.to_json());
    if (!atomic_write(state_directory_ / "history.json", history.dump(2) + "\n", &error))
        return {false, failure(kSkillLockInvalid, error)};
    if (!next.save(state_directory_ / "skills.lock", &error))
        return {false, failure(kSkillLockInvalid, error)};
    history_ = std::move(next_history);
    active_lock_ = next;
    registry_->publish(std::move(entries));
    return {true, json::object(), std::nullopt, active_lock_};
}

SkillLifecycleResult SkillLifecycleManager::publish_lock(const SkillLockfile& lock) {
    std::vector<SkillIndexEntry> entries;
    std::string error;
    for (const auto& locked : lock.packages) {
        auto package = store_.load(locked.package_digest, &error);
        if (!package || package->id != locked.id || package->version.str() != locked.version ||
            package->resource_digests != locked.resource_digests ||
            package->source_uri != locked.source_uri ||
            package->signature_identity != locked.signature_identity ||
            package->archive_digest != locked.archive_digest ||
            package->publisher != locked.publisher || package->key_id != locked.key_id ||
            package->signature_digest != locked.signature_digest ||
            package->sbom_digest != locked.sbom_digest ||
            package->provenance_digest != locked.provenance_digest ||
            package->registry_digest != locked.registry_digest ||
            package->legacy_unsigned != locked.legacy_unsigned)
            return {false, failure(kSkillDigestMismatch,
                error.empty() ? "lockfile package identity does not match the store" : error,
                {{"skill", locked.id}, {"digest", locked.package_digest}})};
        entries.push_back(package_entry(*package));
    }
    if (!lock.save(state_directory_ / "skills.lock", &error))
        return {false, failure(kSkillLockInvalid, error)};
    active_lock_ = lock;
    registry_->publish(std::move(entries));
    return {true, json::object(), std::nullopt, active_lock_};
}

} // namespace agent_framework
