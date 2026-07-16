#ifndef AGENT_SKILL_REFERENCE_HPP
#define AGENT_SKILL_REFERENCE_HPP

#include <agent/skills/skill_resource_access.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace agent_framework {

struct SkillReferenceLimits {
    std::size_t max_page_bytes = 64U * 1024U;
    std::uint64_t max_total_bytes = 1024U * 1024U;
    std::uint64_t max_index_source_bytes = 8U * 1024U * 1024U;
    std::uint64_t max_derived_index_bytes = 16U * 1024U * 1024U;
    std::size_t max_search_hits = 32U;
    std::size_t snippet_bytes = 512U;
};

struct SkillCitation {
    std::string resource_id;
    std::string resource_digest;
    std::string package_digest;
    std::string source_uri;
    std::string license;
    std::string title;
    std::vector<std::string> authors;
    std::string published;
    std::string url;
    std::string locator;
    std::uint64_t byte_start = 0;
    std::uint64_t byte_end = 0;
};

struct SkillReferencePage {
    std::string text;
    std::uint64_t next_offset = 0;
    bool eof = false;
    SkillCitation citation;
};

struct SkillReferenceResult {
    bool ok = false;
    nlohmann::json error = nlohmann::json::object();
    std::optional<SkillReferencePage> page;
};

struct SkillReferenceSearchHit {
    double score = 0.0;
    std::uint64_t match_start = 0;
    std::uint64_t match_end = 0;
    std::uint64_t snippet_start = 0;
    std::uint64_t snippet_end = 0;
    std::string snippet;
    SkillCitation citation;
};

struct SkillReferenceSearchResult {
    bool ok = false;
    nlohmann::json error = nlohmann::json::object();
    std::filesystem::path index_path;
    std::vector<SkillReferenceSearchHit> hits;
};

class SkillReferenceService {
public:
    explicit SkillReferenceService(SkillReferenceLimits limits = {},
                                   std::filesystem::path derived_root = {})
        : limits_(limits), derived_root_(std::move(derived_root)) {}

    SkillReferenceResult read_page(const SkillResourceHandle& handle,
                                   std::uint64_t offset,
                                   std::size_t max_bytes) const;
    SkillReferenceSearchResult search(const SkillResourceHandle& handle,
                                      const std::string& query,
                                      std::size_t max_hits) const;
    std::uint64_t bytes_read() const;

private:
    SkillReferenceLimits limits_;
    std::filesystem::path derived_root_;
    mutable std::mutex mutex_;
    mutable std::uint64_t bytes_read_ = 0;
};

} // namespace agent_framework

#endif
