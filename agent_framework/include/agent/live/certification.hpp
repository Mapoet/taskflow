#pragma once

#include <string>
#include <vector>

#include "agent/contracts/contract.hpp"

namespace agent_framework::live {

struct EnvironmentManifest {
    contracts::ContractMetadata metadata;
    std::string os;
    std::string build_digest;
    std::string git_revision;
    std::string config_digest;
    std::string provider_revision;
    std::string endpoint_class;
    std::string memory_policy_revision;
    std::uint64_t provider_generation{0};
    std::vector<std::string> secret_refs;
};
struct ExecutionAttestation {
    std::string cell_id;
    bool required{true};
    bool executed{false};
    bool passed{false};
    std::string reason;
    std::string started_at;
    std::string finished_at;
    std::vector<std::string> evidence_digests;
};
struct CertificationReport {
    std::string environment_digest;
    std::string issued_at;
    std::string expires_at;
    std::vector<ExecutionAttestation> cells;
    bool certified{false};
    std::vector<std::string> blockers;
};

std::string environment_digest(const EnvironmentManifest& manifest);
CertificationReport certify(const EnvironmentManifest& manifest,
                            std::vector<ExecutionAttestation> attestations,
                            std::string issued_at, std::string expires_at);
bool certification_valid_for(const CertificationReport& report,
                             const EnvironmentManifest& manifest,
                             std::string_view now);

}  // namespace agent_framework::live
