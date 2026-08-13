#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/agent_template/types.hpp"
#include "agent/skills/skill_registry.hpp"

namespace agent_framework::agent_template
{

    struct SkillCandidate
    {
        std::string skill_id;
        std::string version;
        std::string package_digest;
        double score{0.0};
        std::vector<std::string> matched_capabilities;
        std::vector<std::string> matched_terms;
    };
    struct CandidateQuery
    {
        std::string task;
        std::vector<std::string> required_capabilities;
        std::vector<std::string> candidate_ids;
        SkillRole role{SkillRole::Worker};
        std::size_t top_k{10};
    };

    class SkillCandidateRetriever
    {
    public:
        std::vector<SkillCandidate> retrieve(const SkillRegistrySnapshot &snapshot,
                                             const CandidateQuery &query) const;
    };

    struct SessionBuildRequest
    {
        contracts::ContractMetadata metadata;
        std::string session_id;
        SkillCollaborationPlan plan;
        SkillRegistrySnapshot registry;
        PermissionEnvelope parent_permissions;
        std::string model_profiles_digest;
        std::string prompt_revisions_digest;
        std::string deployment_generation;
    };
    struct SessionBuildResult
    {
        std::optional<ActiveSkillSession> session;
        std::vector<contracts::ContractIssue> issues;
    };

    class ActiveSkillSessionBuilder
    {
    public:
        SessionBuildResult build(const SessionBuildRequest &request) const;
    };

    bool permission_is_subset(const PermissionEnvelope &child,
                              const PermissionEnvelope &parent,
                              std::string *reason = nullptr);
    PermissionEnvelope intersect_permissions(const PermissionEnvelope &first,
                                             const PermissionEnvelope &second);

} // namespace agent_framework::agent_template
