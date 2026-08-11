#include "agent/sandbox/workspace.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

#include "agent/contracts/contract.hpp"

namespace agent_framework::sandbox {

std::optional<WorkspaceSnapshot> snapshot_workspace(
    const std::filesystem::path& root_value, std::uint64_t quota, std::string* error) {
    std::error_code ec;
    const auto root = std::filesystem::weakly_canonical(root_value, ec);
    if(ec || !std::filesystem::is_directory(root)) {
        if(error) *error = "workspace root is unavailable";
        return std::nullopt;
    }
    WorkspaceSnapshot out;
    for(std::filesystem::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) {
        if(it->is_symlink(ec)) {
            if(error) *error = "workspace symlinks are not snapshot-safe: " + it->path().string();
            return std::nullopt;
        }
        if(!it->is_regular_file(ec)) continue;
        const auto size = it->file_size(ec);
        if(ec || (quota != 0 && out.total_bytes + size > quota)) {
            if(error) *error = ec ? ec.message() : "workspace quota exceeded";
            return std::nullopt;
        }
        std::ifstream input(it->path(), std::ios::binary);
        std::ostringstream content;
        content << input.rdbuf();
        if(!input.good() && !input.eof()) {
            if(error) *error = "workspace file read failed";
            return std::nullopt;
        }
        WorkspaceEntry entry;
        entry.relative_path = it->path().lexically_relative(root).generic_string();
        entry.size = size;
        entry.digest = contracts::embedded_digest(content.str()).value_or("");
        out.total_bytes += size;
        out.entries.push_back(std::move(entry));
    }
    if(ec) { if(error) *error = ec.message(); return std::nullopt; }
    std::sort(out.entries.begin(), out.entries.end(),
              [](const auto& a, const auto& b) { return a.relative_path < b.relative_path; });
    nlohmann::json basis = nlohmann::json::array();
    for(const auto& entry : out.entries) basis.push_back({entry.relative_path, entry.size, entry.digest});
    out.digest = contracts::embedded_digest(basis).value_or("");
    return out;
}

WorkspaceDiff diff_workspace(const WorkspaceSnapshot& before, const WorkspaceSnapshot& after) {
    WorkspaceDiff out; out.base_digest = before.digest; out.output_digest = after.digest;
    std::map<std::string, std::string> left, right;
    for (const auto& entry : before.entries) left[entry.relative_path] = entry.digest;
    for (const auto& entry : after.entries) right[entry.relative_path] = entry.digest;
    for (const auto& [path, digest] : right) {
        const auto found = left.find(path);
        if (found == left.end()) out.added.push_back(path);
        else if (found->second != digest) out.modified.push_back(path);
    }
    for (const auto& [path, digest] : left) if (!right.contains(path)) out.removed.push_back(path);
    out.digest = contracts::embedded_digest(nlohmann::json{{"base", out.base_digest},
        {"output", out.output_digest}, {"added", out.added}, {"modified", out.modified},
        {"removed", out.removed}}).value_or("");
    return out;
}

}  // namespace agent_framework::sandbox
