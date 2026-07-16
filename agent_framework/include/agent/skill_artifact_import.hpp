#ifndef AGENT_SKILL_ARTIFACT_IMPORT_HPP
#define AGENT_SKILL_ARTIFACT_IMPORT_HPP

#include "skill_resource_cache.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace agent_framework {

class TaskControl;

struct SkillArtifactLimits {
    std::uint64_t max_download_bytes = 256U * 1024U * 1024U;
    std::uint64_t max_expanded_bytes = 512U * 1024U * 1024U;
    std::uint64_t max_entry_bytes = 256U * 1024U * 1024U;
    std::size_t max_entries = 4096U;
    double max_expansion_ratio = 20.0;
};

using SkillArtifactChunkConsumer = std::function<bool(std::string_view)>;
using SkillArtifactReader = std::function<bool(const SkillArtifactChunkConsumer&,
                                                nlohmann::json*)>;

struct SkillArtifactSource {
    std::string id;
    std::uint64_t declared_size = 0;
    std::string expected_sha256;
    SkillArtifactReader reader;
};

enum class SkillArchiveEntryType { Regular, Directory, Symlink, Special };

struct SkillArchiveEntry {
    std::string path;
    SkillArchiveEntryType type = SkillArchiveEntryType::Regular;
    std::uint64_t compressed_size = 0;
    std::uint64_t expanded_size = 0;
    SkillArtifactReader reader;
};

struct SkillArtifactImportResult {
    bool ok = false;
    nlohmann::json error = nlohmann::json::object();
    std::vector<SkillCacheObject> objects;
    std::vector<std::shared_ptr<SkillCacheLease>> leases;
};

class SkillArtifactImporter {
public:
    SkillArtifactImporter(std::shared_ptr<SkillResourceCache> cache,
                          std::filesystem::path staging_root,
                          SkillArtifactLimits limits = {});

    SkillArtifactImportResult import_archive(
        const SkillArtifactSource& source,
        const std::vector<SkillArchiveEntry>& entries,
        TaskControl* control = nullptr) const;

private:
    std::shared_ptr<SkillResourceCache> cache_;
    std::filesystem::path staging_root_;
    SkillArtifactLimits limits_;
};

} // namespace agent_framework

#endif
