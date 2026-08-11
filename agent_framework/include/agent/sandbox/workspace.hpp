#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace agent_framework::sandbox {

struct WorkspaceEntry {
    std::string relative_path;
    std::uint64_t size{0};
    std::string digest;
};
struct WorkspaceSnapshot {
    std::string digest;
    std::uint64_t total_bytes{0};
    std::vector<WorkspaceEntry> entries;
};
struct WorkspaceDiff {
    std::string base_digest;
    std::string output_digest;
    std::vector<std::string> added;
    std::vector<std::string> modified;
    std::vector<std::string> removed;
    std::string digest;
};

std::optional<WorkspaceSnapshot> snapshot_workspace(const std::filesystem::path& root,
                                                    std::uint64_t byte_quota,
                                                    std::string* error = nullptr);
WorkspaceDiff diff_workspace(const WorkspaceSnapshot& before,
                             const WorkspaceSnapshot& after);

}  // namespace agent_framework::sandbox
