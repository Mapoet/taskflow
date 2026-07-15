#ifndef AGENT_SKILL_LIFECYCLE_HPP
#define AGENT_SKILL_LIFECYCLE_HPP

#include "skill_registry.hpp"

#include <filesystem>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace agent_framework {

inline constexpr const char* kSkillLifecycleInvalid = "skill_lifecycle_invalid";
inline constexpr const char* kSkillDependencyConflict = "skill_dependency_conflict";
inline constexpr const char* kSkillLockInvalid = "skill_lock_invalid";
inline constexpr const char* kSkillPackageInUse = "skill_package_in_use";
inline constexpr const char* kSkillDigestMismatch = "skill_digest_mismatch";

struct SkillSemVersion {
    std::uint64_t major = 0;
    std::uint64_t minor = 0;
    std::uint64_t patch = 0;
    std::vector<std::string> prerelease;
    std::string build;

    SkillSemVersion() = default;
    SkillSemVersion(std::uint64_t major_value, std::uint64_t minor_value,
                    std::uint64_t patch_value)
        : major(major_value), minor(minor_value), patch(patch_value) {}

    static std::optional<SkillSemVersion> parse(const std::string& value,
                                                std::string* error = nullptr);
    std::string str() const;
    friend bool operator==(const SkillSemVersion&, const SkillSemVersion&) = default;
    friend bool operator<(const SkillSemVersion& lhs, const SkillSemVersion& rhs);
};

class SkillSemVersionRange {
public:
    static std::optional<SkillSemVersionRange> parse(const std::string& value,
                                                     std::string* error = nullptr);
    bool contains(const SkillSemVersion& version) const;
    const std::string& expression() const noexcept { return expression_; }

private:
    struct Comparator {
        enum class Op { Eq, Lt, Lte, Gt, Gte } op = Op::Eq;
        SkillSemVersion version;
    };
    std::string expression_;
    std::vector<std::vector<Comparator>> alternatives_;
};

struct SkillPackageRecord {
    std::string id;
    SkillSemVersion version;
    std::string package_digest;
    std::map<std::string, std::string> resource_digests;
    std::string source_uri;
    std::string signature_identity;
    std::filesystem::path package_path;
    std::shared_ptr<const SkillManifest> manifest;
    std::shared_ptr<const void> lease;
};

struct SkillDependencyRequirement {
    std::string requester;
    std::string skill_id;
    std::string range;
    bool optional = false;
    std::vector<std::string> path;
};

struct SkillDependencyResolution {
    bool ok = false;
    std::map<std::string, SkillPackageRecord> packages;
    std::vector<SkillDependencyRequirement> conflict;
    nlohmann::json error = nlohmann::json::object();
};

class SkillDependencyResolver {
public:
    using Catalog = std::map<std::string, std::vector<SkillPackageRecord>>;
    explicit SkillDependencyResolver(Catalog catalog) : catalog_(std::move(catalog)) {}
    SkillDependencyResolution resolve(const std::vector<SkillDependencyRequirement>& roots) const;

private:
    Catalog catalog_;
};

struct SkillLockPackage {
    std::string id;
    std::string version;
    std::string package_digest;
    std::map<std::string, std::string> resource_digests;
    std::string source_uri;
    std::string signature_identity;
};

struct SkillLockEdge {
    std::string from;
    std::string to;
    std::string range;
    bool optional = false;
};

struct SkillLockfile {
    std::vector<std::string> roots;
    std::map<std::string, std::string> root_ranges;
    std::vector<SkillLockPackage> packages;
    std::vector<SkillLockEdge> edges;

    nlohmann::json to_json() const;
    static std::optional<SkillLockfile> from_json(const nlohmann::json& value,
                                                  std::string* error = nullptr);
    std::string dump() const;
    bool save(const std::filesystem::path& path, std::string* error = nullptr) const;
    static std::optional<SkillLockfile> load(const std::filesystem::path& path,
                                             std::string* error = nullptr);
};

struct SkillLifecycleResult {
    bool ok = false;
    nlohmann::json error = nlohmann::json::object();
    std::optional<SkillPackageRecord> package;
    std::optional<SkillLockfile> lockfile;

    SkillLifecycleResult() = default;
    SkillLifecycleResult(bool success, nlohmann::json failure = nlohmann::json::object(),
                         std::optional<SkillPackageRecord> package_value = std::nullopt,
                         std::optional<SkillLockfile> lock_value = std::nullopt)
        : ok(success), error(std::move(failure)), package(std::move(package_value)),
          lockfile(std::move(lock_value)) {}
};

struct SkillInstallOptions {
    std::string source_uri;
    std::string signature_identity;
};

class SkillPackageStore {
public:
    explicit SkillPackageStore(std::filesystem::path root);
    SkillLifecycleResult import_package(const std::filesystem::path& package,
                                        const SkillInstallOptions& options = {});
    std::optional<SkillPackageRecord> load(const std::string& digest,
                                           std::string* error = nullptr) const;
    std::vector<SkillPackageRecord> catalog(std::string* error = nullptr) const;
    bool remove(const std::string& digest, std::string* error = nullptr);
    const std::filesystem::path& root() const noexcept { return root_; }

private:
    std::filesystem::path root_;
    mutable std::mutex lease_mutex_;
    mutable std::map<std::string, std::weak_ptr<const void>> leases_;
};

SkillLifecycleResult inspect_skill_package(const std::filesystem::path& package);

class SkillLifecycleManager {
public:
    SkillLifecycleManager(std::shared_ptr<SkillRegistry> registry,
                          std::filesystem::path store_root);

    SkillLifecycleResult install(const std::filesystem::path& package,
                                 const SkillInstallOptions& options = {});
    SkillLifecycleResult enable(const std::string& skill_id,
                                const std::string& range = "*");
    SkillLifecycleResult disable(const std::string& skill_id);
    SkillLifecycleResult update(const std::filesystem::path& package,
                                const SkillInstallOptions& options = {});
    SkillLifecycleResult rollback(const std::string& skill_id);
    SkillLifecycleResult remove(const std::string& package_digest);
    SkillLifecycleResult reload();
    SkillLockfile lockfile() const;

private:
    std::shared_ptr<SkillRegistry> registry_;
    SkillPackageStore store_;
    std::filesystem::path state_directory_;
    SkillLockfile active_lock_;
    std::vector<SkillLockfile> history_;
    std::map<std::string, std::string> requested_roots_;
    mutable std::mutex mutex_;

    SkillLifecycleResult publish_resolved();
    SkillLifecycleResult publish_lock(const SkillLockfile& lock);
    bool recover(std::string* error);
};

std::optional<std::string> skill_sha256_file(const std::filesystem::path& path,
                                             std::string* error = nullptr);

} // namespace agent_framework

#endif
