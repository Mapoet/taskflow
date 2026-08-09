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

std::optional<WorkspaceSnapshot> snapshot_workspace(const std::filesystem::path& root,
                                                    std::uint64_t byte_quota,
                                                    std::string* error = nullptr);

}  // namespace agent_framework::sandbox
