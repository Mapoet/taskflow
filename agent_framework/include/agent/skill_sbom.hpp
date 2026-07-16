#ifndef AGENT_SKILL_SBOM_HPP
#define AGENT_SKILL_SBOM_HPP

#include "skill_archive.hpp"
#include "skill_lifecycle.hpp"
#include "skill_supply_chain.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace agent_framework {

struct SkillProvenanceOptions {
    std::string source_uri;
    std::string source_revision;
    std::string builder_id = "agent.taskflow/skillctl";
};

struct SkillAuditedArchiveResult {
    bool ok = false;
    std::string error;
    SkillArchiveResult archive;
    SkillPackageMetadata metadata;
    nlohmann::json sbom;
    nlohmann::json provenance;
};

nlohmann::json generate_skill_sbom(const SkillPackageRecord& package);
nlohmann::json generate_skill_provenance(const SkillPackageRecord& package,
                                         const SkillProvenanceOptions& options);

SkillAuditedArchiveResult build_audited_skill_archive(
    const std::filesystem::path& source_directory,
    const std::filesystem::path& output_archive,
    const SkillProvenanceOptions& provenance,
    const SkillArchiveLimits& limits = {});

SkillAuditedArchiveResult inspect_audited_skill_archive(
    const std::filesystem::path& archive,
    const SkillPackageMetadata& expected,
    const SkillArchiveLimits& limits = {});

} // namespace agent_framework

#endif
