#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace agent_framework::execution {

enum class ArtifactActionKind { WriteText };

struct ArtifactAction {
    std::string action_id;
    ArtifactActionKind kind{ArtifactActionKind::WriteText};
    std::filesystem::path relative_path;
    std::string content;
    std::string idempotency_key;
    bool approved{false};
};

struct ArtifactEntry {
    std::string relative_path;
    std::string content_digest;
    std::uint64_t size{0};
    std::string producer_action_id;
};

struct ArtifactManifest {
    std::string run_id;
    std::string parent_manifest_digest;
    std::vector<ArtifactEntry> artifacts;
    std::string workspace_diff_digest;
    std::string manifest_digest;
};

struct ArtifactExecutionReceipt {
    bool succeeded{false};
    bool replayed{false};
    std::string action_id;
    std::string relative_path;
    std::string idempotency_key;
    std::string effect_digest;
    std::string preimage_digest;
    ArtifactManifest manifest;
    std::string error_code;
    std::string error_message;
};

struct ArtifactRequirement {
    std::string requirement_id;
    std::filesystem::path relative_path;
    bool nonempty{true};
    std::optional<std::string> expected_digest;
};

struct OracleObservation {
    std::string oracle_id;
    std::string requirement_id;
    bool passed{false};
    std::string artifact_manifest_digest;
    std::string observed_digest;
    std::string finding_id;
    std::string detail;
};

class WorkspaceArtifactExecutor {
public:
    explicit WorkspaceArtifactExecutor(std::filesystem::path workspace_root);
    ArtifactExecutionReceipt execute(std::string run_id, const ArtifactAction& action,
                                     const ArtifactManifest* parent = nullptr);
    bool rollback(const ArtifactExecutionReceipt& receipt, std::string* error = nullptr);

private:
    struct Journal { ArtifactExecutionReceipt receipt; std::optional<std::string> preimage; };
    std::filesystem::path resolve(const std::filesystem::path& relative,
                                  std::string* error) const;
    ArtifactManifest scan(std::string run_id, std::string parent_digest,
                          std::string producer) const;
    std::filesystem::path root_;
    std::map<std::string, Journal> journal_;
};

class FilesystemArtifactOracle {
public:
    explicit FilesystemArtifactOracle(std::filesystem::path workspace_root);
    std::vector<OracleObservation> verify(const ArtifactManifest& manifest,
                                          const std::vector<ArtifactRequirement>& requirements) const;
    static bool reusable(const OracleObservation& observation,
                         std::string_view current_manifest_digest);
private:
    std::filesystem::path root_;
};

}  // namespace agent_framework::execution
