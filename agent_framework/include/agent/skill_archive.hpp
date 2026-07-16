#ifndef AGENT_SKILL_ARCHIVE_HPP
#define AGENT_SKILL_ARCHIVE_HPP

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace agent_framework {

struct SkillArchiveLimits {
    std::uint64_t max_archive_bytes = 512ull * 1024 * 1024;
    std::uint64_t max_expanded_bytes = 512ull * 1024 * 1024;
    std::uint64_t max_entry_bytes = 256ull * 1024 * 1024;
    std::uint32_t max_entries = 4096;
};

struct SkillArchiveMember {
    std::string path;
    std::uint64_t size = 0;
    std::uint32_t crc32 = 0;
    bool executable = false;
};

struct SkillArchiveResult {
    bool ok = false;
    std::string error;
    std::string archive_digest;
    std::vector<SkillArchiveMember> entries;
};

SkillArchiveResult build_skill_archive(
    const std::filesystem::path& source_directory,
    const std::filesystem::path& output_archive,
    const std::map<std::string, std::string>& generated_files = {},
    const SkillArchiveLimits& limits = {});

SkillArchiveResult inspect_skill_archive(const std::filesystem::path& archive,
                                         const SkillArchiveLimits& limits = {});

SkillArchiveResult extract_skill_archive(const std::filesystem::path& archive,
                                         const std::filesystem::path& output_directory,
                                         const SkillArchiveLimits& limits = {});

} // namespace agent_framework

#endif
