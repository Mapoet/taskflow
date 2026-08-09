#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "agent/contracts/contract.hpp"

namespace agent_framework::sandbox {

struct SandboxSpec {
    contracts::ContractMetadata metadata;
    std::string provider;
    std::string image_digest;
    std::string workspace_base_digest;
    std::vector<std::string> command;
    std::vector<std::string> read_only_mounts;
    std::vector<std::string> writable_mounts;
    std::vector<std::string> network_allowlist;
    std::vector<std::string> credential_refs;
    std::uint64_t cpu_millis{0};
    std::uint64_t memory_bytes{0};
    std::uint64_t wall_time_ms{0};
    std::string policy_revision;
    std::string memory_view_digest;
};

struct SandboxManifest {
    contracts::ContractMetadata metadata;
    std::string sandbox_id;
    std::string spec_digest;
    std::string provider_version;
    std::string workspace_input_digest;
    std::string workspace_output_digest;
    std::string stdout_digest;
    std::string stderr_digest;
    int exit_code{0};
    std::uint64_t cpu_millis{0};
    std::uint64_t peak_memory_bytes{0};
    std::uint64_t wall_time_ms{0};
    std::string started_at;
    std::string finished_at;
};

nlohmann::json encode(const SandboxSpec& value);
nlohmann::json encode(const SandboxManifest& value);
std::optional<SandboxSpec> decode_sandbox_spec(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);
std::optional<SandboxManifest> decode_sandbox_manifest(
    const nlohmann::json& value, const contracts::ParseContext& context = {},
    std::vector<contracts::ContractIssue>* issues = nullptr);

}  // namespace agent_framework::sandbox
