#ifndef AGENT_SKILL_LIFECYCLE_HPP
#define AGENT_SKILL_LIFECYCLE_HPP

#include <agent/skills/skill_audit.hpp>

#include <agent/skills/skill_registry.hpp>
#include <agent/skills/skill_supply_chain.hpp>

#include <filesystem>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
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
    std::string archive_digest;
    std::string publisher;
    std::string key_id;
    std::string signature_digest;
    std::string sbom_digest;
    std::string provenance_digest;
    std::string registry_digest;
    bool legacy_unsigned = true;
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
    std::string archive_digest;
    std::string publisher;
    std::string key_id;
    std::string signature_digest;
    std::string sbom_digest;
    std::string provenance_digest;
    std::string registry_digest;
    bool legacy_unsigned = true;

    SkillLockPackage() = default;
    SkillLockPackage(std::string id_value, std::string version_value,
                     std::string package_digest_value,
                     std::map<std::string, std::string> resource_digests_value,
                     std::string source_uri_value, std::string signature_identity_value)
        : id(std::move(id_value)), version(std::move(version_value)),
          package_digest(std::move(package_digest_value)),
          resource_digests(std::move(resource_digests_value)),
          source_uri(std::move(source_uri_value)),
          signature_identity(std::move(signature_identity_value)) {}
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
    std::optional<SkillSignatureEnvelope> signature;
    SkillTrustStore trust;
    bool remote = false;
    bool allow_unsigned_local = false;
    std::int64_t verification_time = 0;
    std::filesystem::path archive_path;
    std::string archive_digest;
    std::string publisher;
    std::string key_id;
    std::string signature_digest;
    std::string sbom_digest;
    std::string provenance_digest;
    std::string registry_digest;
    bool legacy_unsigned = true;

    SkillInstallOptions() = default;
    SkillInstallOptions(std::string source_uri_value, std::string signature_identity_value)
        : source_uri(std::move(source_uri_value)),
          signature_identity(std::move(signature_identity_value)) {}
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
                          std::filesystem::path store_root,
                          SkillAuditSink audit_sink = {});

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
    SkillAuditSink audit_sink_;

    SkillLifecycleResult publish_resolved();
    SkillLifecycleResult publish_lock(const SkillLockfile& lock);
    bool recover(std::string* error);
    SkillLifecycleResult audited(std::string action, std::string target,
                                 SkillLifecycleResult result) const;
};

std::optional<std::string> skill_sha256_file(const std::filesystem::path& path,
                                             std::string* error = nullptr);

} // namespace agent_framework

#endif
