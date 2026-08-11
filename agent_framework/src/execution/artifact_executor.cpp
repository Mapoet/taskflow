#include "agent/execution/artifact_executor.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "agent/skills/skill_supply_chain.hpp"

namespace fs = std::filesystem;
namespace agent_framework::execution {
namespace {
std::string digest(std::string_view bytes) {
    return "sha256:" + skill_sha256_bytes(std::string(bytes)).value_or("unavailable");
}
std::optional<std::string> read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if(!in) return std::nullopt;
    std::ostringstream out; out << in.rdbuf(); return out.str();
}
bool below(const fs::path& root, const fs::path& candidate) {
    auto r = root.begin(), c = candidate.begin();
    for(; r != root.end(); ++r, ++c) if(c == candidate.end() || *r != *c) return false;
    return true;
}
fs::path jailed(const fs::path& root, const fs::path& relative, std::string* error) {
    if(relative.empty() || relative.is_absolute()) {
        if(error) *error = "artifact path must be non-empty and relative";
        return {};
    }
    std::error_code ec;
    const auto parent = fs::weakly_canonical(root / relative.parent_path(), ec);
    if(ec || !below(root, parent)) {
        if(error) *error = "artifact path escapes workspace";
        return {};
    }
    const auto target = parent / relative.filename();
    if(fs::is_symlink(fs::symlink_status(target, ec))) {
        if(error) *error = "symbolic-link artifact targets are forbidden";
        return {};
    }
    return target;
}
}

WorkspaceArtifactExecutor::WorkspaceArtifactExecutor(fs::path root) {
    fs::create_directories(root);
    root_ = fs::weakly_canonical(root);
}

fs::path WorkspaceArtifactExecutor::resolve(const fs::path& relative, std::string* error) const {
    return jailed(root_, relative, error);
}

ArtifactManifest WorkspaceArtifactExecutor::scan(std::string run_id, std::string parent,
                                                  std::string producer) const {
    ArtifactManifest result;
    result.run_id = std::move(run_id);
    result.parent_manifest_digest = std::move(parent);
    std::vector<fs::path> paths;
    for(const auto& item : fs::recursive_directory_iterator(root_))
        if(!item.is_symlink() && item.is_regular_file()) paths.push_back(fs::relative(item.path(), root_));
    std::sort(paths.begin(), paths.end());
    nlohmann::json canonical = nlohmann::json::array();
    for(const auto& relative : paths) {
        auto bytes = read(root_ / relative).value_or("");
        ArtifactEntry entry{relative.generic_string(), digest(bytes), bytes.size(), producer};
        result.artifacts.push_back(entry);
        canonical.push_back({{"path", entry.relative_path}, {"digest", entry.content_digest},
                             {"size", entry.size}});
    }
    result.workspace_diff_digest = digest(canonical.dump());
    result.manifest_digest = digest(nlohmann::json{{"run_id", result.run_id},
        {"parent", result.parent_manifest_digest}, {"artifacts", canonical},
        {"workspace_diff", result.workspace_diff_digest}}.dump());
    return result;
}

ArtifactExecutionReceipt WorkspaceArtifactExecutor::execute(
    std::string run_id, const ArtifactAction& action, const ArtifactManifest* parent) {
    ArtifactExecutionReceipt out;
    out.action_id = action.action_id; out.relative_path = action.relative_path.generic_string();
    out.idempotency_key = action.idempotency_key;
    if(!action.approved) { out.error_code = "action_not_approved"; return out; }
    if(action.action_id.empty() || action.idempotency_key.empty()) {
        out.error_code = "invalid_action_identity"; return out;
    }
    if(auto found = journal_.find(action.idempotency_key); found != journal_.end()) {
        out = found->second.receipt; out.replayed = true; return out;
    }
    std::string error;
    auto target = resolve(action.relative_path, &error);
    if(target.empty()) { out.error_code = "workspace_escape"; out.error_message = error; return out; }
    auto preimage = read(target);
    out.preimage_digest = preimage ? digest(*preimage) : digest("absent");
    fs::create_directories(target.parent_path());
    auto temporary = target; temporary += ".phase4-tmp";
    { std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
      if(!file || !(file << action.content)) { out.error_code = "artifact_write_failed"; return out; } }
    std::error_code ec; fs::rename(temporary, target, ec);
    if(ec) { fs::remove(temporary); out.error_code = "artifact_commit_failed"; out.error_message = ec.message(); return out; }
    out.succeeded = true;
    out.effect_digest = digest(action.action_id + "\n" + action.relative_path.generic_string() + "\n" + action.content);
    out.manifest = scan(std::move(run_id), parent ? parent->manifest_digest : "", action.action_id);
    if(parent) {
        for(auto& entry : out.manifest.artifacts) {
            if(entry.relative_path == action.relative_path.generic_string()) continue;
            const auto previous = std::find_if(parent->artifacts.begin(), parent->artifacts.end(),
                [&](const auto& item) { return item.relative_path == entry.relative_path &&
                                               item.content_digest == entry.content_digest; });
            if(previous != parent->artifacts.end()) entry.producer_action_id = previous->producer_action_id;
        }
    }
    journal_.emplace(action.idempotency_key, Journal{out, preimage});
    return out;
}

bool WorkspaceArtifactExecutor::rollback(const ArtifactExecutionReceipt& receipt, std::string* error) {
    auto found = journal_.find(receipt.idempotency_key);
    if(found == journal_.end()) { if(error) *error = "receipt not found"; return false; }
    auto target = resolve(found->second.receipt.relative_path, error); if(target.empty()) return false;
    if(found->second.preimage) { std::ofstream file(target, std::ios::binary | std::ios::trunc); file << *found->second.preimage; return !!file; }
    std::error_code ec; fs::remove(target, ec); return !ec;
}

FilesystemArtifactOracle::FilesystemArtifactOracle(fs::path root)
    : root_(fs::weakly_canonical(std::move(root))) {}

std::vector<OracleObservation> FilesystemArtifactOracle::verify(
    const ArtifactManifest& manifest, const std::vector<ArtifactRequirement>& requirements) const {
    std::vector<OracleObservation> out;
    for(const auto& requirement : requirements) {
        OracleObservation item;
        item.oracle_id = "filesystem-v1";
        item.requirement_id = requirement.requirement_id;
        item.artifact_manifest_digest = manifest.manifest_digest;
        std::string jail_error;
        auto path = jailed(root_, requirement.relative_path, &jail_error);
        auto bytes = path.empty() ? std::optional<std::string>{} : read(path);
        if(path.empty()) item.detail = jail_error;
        else if(!bytes) item.detail = "required artifact is missing";
        else {
            item.observed_digest = digest(*bytes);
            const auto listed = std::find_if(manifest.artifacts.begin(), manifest.artifacts.end(),
                [&](const auto& e) { return e.relative_path == requirement.relative_path.generic_string() &&
                                             e.content_digest == item.observed_digest; });
            if(listed == manifest.artifacts.end()) item.detail = "artifact is absent from or differs from manifest";
            else if(requirement.nonempty && bytes->empty()) item.detail = "artifact is empty";
            else if(requirement.expected_digest && *requirement.expected_digest != item.observed_digest)
                item.detail = "artifact digest does not match requirement";
            else item.passed = true;
        }
        if(!item.passed) item.finding_id = "artifact:" + requirement.requirement_id;
        out.push_back(std::move(item));
    }
    return out;
}

bool FilesystemArtifactOracle::reusable(const OracleObservation& observation,
                                        std::string_view current_manifest_digest) {
    return observation.passed && observation.artifact_manifest_digest == current_manifest_digest;
}
}  // namespace agent_framework::execution
