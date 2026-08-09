#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "agent/contracts/contract.hpp"

namespace agent_framework::memory_v2 {

enum class MemoryLevel { System, Organization, Principal, Project, Task, Turn };
enum class Authority { Authoritative, Verified, Candidate, Observed, Derived };
enum class MemoryKind {
    Instruction,
    Semantic,
    Episodic,
    Procedural,
    Evidentiary,
    Working,
    Operational,
    Conversational
};
enum class MemoryStatus { Raw, Candidate, Verified, Authoritative, Rejected, Superseded, Tombstoned, Expired };

struct MemoryScope {
    std::string tenant_id;
    std::string organization_id;
    std::string principal_id;
    std::string agent_id;
    std::string project_id;
    std::string workspace_id;
    std::string path_scope;
    std::string task_id;
    std::string run_id;
    std::string turn_id;
    MemoryLevel level{MemoryLevel::Task};
};

struct MemoryRecord {
    contracts::ContractMetadata metadata;
    std::string record_id;
    std::uint64_t revision{1};
    MemoryScope scope;
    MemoryKind kind{MemoryKind::Semantic};
    Authority authority{Authority::Candidate};
    MemoryStatus status{MemoryStatus::Candidate};
    std::string source_kind;
    std::string source_locator;
    std::string source_digest;
    std::string content_type;
    nlohmann::json content = nlohmann::json::object();
    std::string trust_class;
    std::string freshness_deadline;
    std::string sensitivity;
    std::string purpose;
    std::vector<std::string> acl_principals;
    std::vector<std::string> supports;
    std::vector<std::string> conflicts_with;
    std::string supersedes_id;
    std::string event_time;
    std::string ingest_time;
    std::string valid_from;
    std::string valid_until;
    std::string retention_until;
    bool legal_hold{false};
    std::string created_at;
};

struct MemorySnapshot {
    contracts::ContractMetadata metadata;
    std::string snapshot_id;
    std::uint64_t provider_generation{0};
    std::vector<std::string> record_revision_digests;
    std::string policy_revision;
    std::string created_at;
};

struct MemoryViewSpec {
    contracts::ContractMetadata metadata;
    std::string workflow_phase;
    MemoryScope subject;
    std::vector<MemoryLevel> allowed_levels;
    std::vector<MemoryKind> allowed_kinds;
    Authority authority_floor{Authority::Derived};
    std::vector<std::string> mandatory_record_ids;
    std::uint64_t byte_budget{0};
    std::uint64_t token_budget{0};
    std::string conflict_policy;
    bool include_procedural_skills{true};
    bool exclude_unverified_executor_claims{false};
};

struct MemorySelection {
    std::string record_id;
    std::uint64_t revision{0};
    std::string digest;
    std::string reason;
    std::uint64_t bytes{0};
};

struct MemoryViewManifest {
    contracts::ContractMetadata metadata;
    std::string snapshot_id;
    std::string snapshot_digest;
    std::string view_spec_digest;
    std::string view_digest;
    std::string policy_revision;
    std::uint64_t provider_generation{0};
    std::vector<MemorySelection> selected;
    std::vector<MemorySelection> excluded;
    std::vector<std::string> conflict_ids;
    std::vector<std::string> redactions;
};

struct MemoryConflict {
    contracts::ContractMetadata metadata;
    std::string conflict_id;
    std::vector<std::string> record_revision_digests;
    std::string resolution_state;
    std::string resolution_reason;
    std::string approval_id;
};

nlohmann::json encode(const MemoryRecord& value);
nlohmann::json encode(const MemorySnapshot& value);
nlohmann::json encode(const MemoryViewSpec& value);
nlohmann::json encode(const MemoryViewManifest& value);
nlohmann::json encode(const MemoryConflict& value);

std::optional<MemoryRecord> decode_memory_record(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<MemorySnapshot> decode_memory_snapshot(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<MemoryViewSpec> decode_memory_view_spec(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<MemoryViewManifest> decode_memory_view_manifest(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<MemoryConflict> decode_memory_conflict(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);

}  // namespace agent_framework::memory_v2
