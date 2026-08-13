#include "agent/agent_template/session.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <unordered_map>

namespace agent_framework::agent_template
{
    namespace
    {
        std::string lower(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
                           { return static_cast<char>(std::tolower(c)); });
            return value;
        }
        bool contains(std::string_view text, std::string_view needle) { return lower(std::string(text)).find(lower(std::string(needle))) != std::string::npos; }
        template <typename T>
        std::vector<T> intersection(const std::vector<T> &a, const std::vector<T> &b)
        {
            std::vector<T> o;
            for (const auto &x : a)
                if (std::find(b.begin(), b.end(), x) != b.end() && std::find(o.begin(), o.end(), x) == o.end())
                    o.push_back(x);
            std::sort(o.begin(), o.end());
            return o;
        }
        template <typename T>
        bool subset(const std::vector<T> &a, const std::vector<T> &b, std::string_view kind, std::string *reason)
        {
            for (const auto &x : a)
                if (std::find(b.begin(), b.end(), x) == b.end())
                {
                    if (reason)
                        *reason = std::string(kind) + ":" + x;
                    return false;
                }
            return true;
        }
        PermissionEnvelope requested(const SkillManifest &m) { return {m.permissions.tools, m.permissions.network, m.permissions.filesystem_read, m.permissions.filesystem_write, m.permissions.environment, m.permissions.secrets}; }
        std::vector<std::string> capabilities(const SkillIndexEntry &e)
        {
            std::vector<std::string> o = e.tags;
            o.insert(o.end(), e.allowed_tools.begin(), e.allowed_tools.end());
            if (e.manifest)
                for (const auto &r : e.manifest->resources)
                    o.push_back(r.id);
            return o;
        }
        void issue(SessionBuildResult &out, std::string code, std::string path, std::string message) { out.issues.push_back({std::move(code), std::move(path), std::move(message)}); }
    }
    bool permission_is_subset(const PermissionEnvelope &c, const PermissionEnvelope &p, std::string *r) { return subset(c.tools, p.tools, "tool", r) && subset(c.network, p.network, "network", r) && subset(c.filesystem_read, p.filesystem_read, "filesystem_read", r) && subset(c.filesystem_write, p.filesystem_write, "filesystem_write", r) && subset(c.environment, p.environment, "environment", r) && subset(c.secrets, p.secrets, "secret", r); }
    PermissionEnvelope intersect_permissions(const PermissionEnvelope &a, const PermissionEnvelope &b) { return {intersection(a.tools, b.tools), intersection(a.network, b.network), intersection(a.filesystem_read, b.filesystem_read), intersection(a.filesystem_write, b.filesystem_write), intersection(a.environment, b.environment), intersection(a.secrets, b.secrets)}; }

    std::vector<SkillCandidate> SkillCandidateRetriever::retrieve(const SkillRegistrySnapshot &snapshot, const CandidateQuery &q) const
    {
        std::vector<SkillCandidate> out;
        if (q.top_k == 0)
            return out;
        for (const auto &e : snapshot.entries())
        {
            if (e.disable_model_invocation && q.candidate_ids.empty())
                continue;
            if (!q.candidate_ids.empty() && std::find(q.candidate_ids.begin(), q.candidate_ids.end(), e.id) == q.candidate_ids.end())
                continue;
            const auto caps = capabilities(e);
            SkillCandidate c{e.id, e.version, e.package_digest, 0, {}, {}};
            bool missing = false;
            for (const auto &required : q.required_capabilities)
            {
                auto it = std::find_if(caps.begin(), caps.end(), [&](const auto &x)
                                       { return lower(x) == lower(required); });
                if (it == caps.end())
                {
                    missing = true;
                    break;
                }
                c.matched_capabilities.push_back(required);
                c.score += 20;
            }
            if (missing)
                continue;
            for (const auto &term : e.trigger_keywords)
                if (contains(q.task, term))
                {
                    c.matched_terms.push_back(term);
                    c.score += 5;
                }
            for (const auto &tag : e.tags)
                if (contains(q.task, tag))
                {
                    c.matched_terms.push_back(tag);
                    c.score += 2;
                }
            if (contains(q.task, e.name) || contains(q.task, e.id))
                c.score += 3;
            if (!q.candidate_ids.empty())
                c.score += 10;
            if (q.role == SkillRole::Verifier && (contains(e.id, "verif") || contains(e.description, "verif")))
                c.score += 4;
            out.push_back(std::move(c));
        }
        std::sort(out.begin(), out.end(), [](const auto &a, const auto &b)
                  { return a.score != b.score ? a.score > b.score : a.skill_id < b.skill_id; });
        if (out.size() > q.top_k)
            out.resize(q.top_k);
        return out;
    }

    SessionBuildResult ActiveSkillSessionBuilder::build(const SessionBuildRequest &r) const
    {
        SessionBuildResult out;
        if (r.session_id.empty())
        {
            issue(out, "session_id_missing", "/session_id", "session id is required");
            return out;
        }
        if (!r.registry.valid())
        {
            issue(out, "registry_snapshot_invalid", "/registry", "valid immutable registry snapshot required");
            return out;
        }
        std::map<std::string, PinnedSkill> pins;
        std::set<std::string> visiting;
        std::function<bool(const std::string &, const std::string &)> pin = [&](const std::string &id, const std::string &path)
        {if(pins.count(id))return true;if(!visiting.insert(id).second){issue(out,"dependency_cycle",path,"skill dependency cycle");return false;}auto e=r.registry.get(id);if(!e||!e->manifest){issue(out,"skill_unavailable",path,"skill not present in pinned registry snapshot: "+id);visiting.erase(id);return false;}PinnedSkill p;p.skill_id=e->id;p.version=e->version;p.package_digest=e->package_digest;p.capability_snapshot_digest=contracts::embedded_digest(nlohmann::json(capabilities(*e))).value_or("");auto req=requested(*e->manifest);p.effective_permissions=intersect_permissions(req,r.parent_permissions);std::string reason;if(!permission_is_subset(p.effective_permissions,r.parent_permissions,&reason)){issue(out,"permission_escalation",path,reason);visiting.erase(id);return false;}for(const auto&d:e->manifest->dependencies){if(d.optional&&!r.registry.get(d.name))continue;auto dep=r.registry.get(d.name);if(!dep){issue(out,"dependency_missing",path,"missing dependency: "+d.name);visiting.erase(id);return false;}if(!d.version.empty()&&d.version!=dep->version){issue(out,"dependency_version_mismatch",path,"dependency version mismatch: "+d.name);visiting.erase(id);return false;}if(!pin(d.name,path+"/dependencies/"+d.name)){visiting.erase(id);return false;}p.dependency_lock[d.name]=dep->version;}pins.emplace(id,std::move(p));visiting.erase(id);return true; };
        for (std::size_t i = 0; i < r.plan.nodes.size(); ++i)
        {
            const auto &n = r.plan.nodes[i];
            if (!n.resolved_skill_id)
            {
                issue(out, "skill_unresolved", "/plan/nodes/" + std::to_string(i), "plan node is not resolved");
                continue;
            }
            if (!pin(*n.resolved_skill_id, "/plan/nodes/" + std::to_string(i)))
                continue;
            auto e = r.registry.get(*n.resolved_skill_id);
            if (e && (!n.resolved_skill_version.empty() && n.resolved_skill_version != e->version || !n.resolved_skill_digest.empty() && n.resolved_skill_digest != e->package_digest))
                issue(out, "skill_pin_mismatch", "/plan/nodes/" + std::to_string(i), "resolved skill pin differs from registry snapshot");
        }
        if (!out.issues.empty())
            return out;
        ActiveSkillSession s;
        s.metadata = r.metadata;
        s.session_id = r.session_id;
        s.plan_digest = encode(r.plan).at("canonical_digest");
        s.registry_generation = std::to_string(r.registry.generation());
        for (auto &[_, p] : pins)
            s.skills.push_back(std::move(p));
        s.effective_permissions = r.parent_permissions;
        s.budget = r.plan.budget;
        s.model_profiles_digest = r.model_profiles_digest;
        s.prompt_revisions_digest = r.prompt_revisions_digest;
        s.capability_snapshot_digest = contracts::embedded_digest(nlohmann::json{{"generation", s.registry_generation}, {"skills", s.skills.size()}}).value_or("");
        s.deployment_generation = r.deployment_generation;
        out.session = std::move(s);
        return out;
    }
} // namespace agent_framework::agent_template
