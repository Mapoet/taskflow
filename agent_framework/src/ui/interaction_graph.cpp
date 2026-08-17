#include "agent/ui/interaction_graph.hpp"

#include <array>
#include <cctype>
#include <set>
#include <stdexcept>

namespace agent_framework::ui
{
    using namespace std::string_view_literals;
    namespace
    {
        template <class E, std::size_t N>
        std::string_view enum_name(E value, const std::array<std::pair<E, std::string_view>, N> &values) noexcept
        {
            for (const auto &[item, label] : values)
                if (item == value)
                    return label;
            return "unknown";
        }
        template <class E, std::size_t N>
        std::optional<E> parse_enum(std::string_view value, const std::array<std::pair<E, std::string_view>, N> &values) noexcept
        {
            for (const auto &[item, label] : values)
                if (label == value)
                    return item;
            return std::nullopt;
        }
        constexpr std::array node_names{
            std::pair{InteractionNodeKind::Message, "message"sv}, std::pair{InteractionNodeKind::Thinking, "thinking"sv},
            std::pair{InteractionNodeKind::Understanding, "understanding"sv}, std::pair{InteractionNodeKind::Decision, "decision"sv},
            std::pair{InteractionNodeKind::CognitionStage, "cognition_stage"sv}, std::pair{InteractionNodeKind::Plan, "plan"sv},
            std::pair{InteractionNodeKind::PlanNode, "plan_node"sv}, std::pair{InteractionNodeKind::MemoryView, "memory_view"sv},
            std::pair{InteractionNodeKind::Agent, "agent"sv}, std::pair{InteractionNodeKind::SkillNode, "skill_node"sv},
            std::pair{InteractionNodeKind::ToolInvocation, "tool_invocation"sv}, std::pair{InteractionNodeKind::Approval, "approval"sv},
            std::pair{InteractionNodeKind::Evidence, "evidence"sv}, std::pair{InteractionNodeKind::Finding, "finding"sv},
            std::pair{InteractionNodeKind::Artifact, "artifact"sv}, std::pair{InteractionNodeKind::Closure, "closure"sv}};
        constexpr std::array edge_names{
            std::pair{InteractionEdgeKind::OriginatedFrom, "originated_from"sv}, std::pair{InteractionEdgeKind::PlannedBy, "planned_by"sv},
            std::pair{InteractionEdgeKind::Implements, "implements"sv}, std::pair{InteractionEdgeKind::DelegatedTo, "delegated_to"sv},
            std::pair{InteractionEdgeKind::ParentOf, "parent_of"sv}, std::pair{InteractionEdgeKind::UsedMemoryView, "used_memory_view"sv},
            std::pair{InteractionEdgeKind::RequestedApproval, "requested_approval"sv}, std::pair{InteractionEdgeKind::Resumes, "resumes"sv},
            std::pair{InteractionEdgeKind::Produced, "produced"sv}, std::pair{InteractionEdgeKind::VerifiedBy, "verified_by"sv},
            std::pair{InteractionEdgeKind::Supports, "supports"sv}, std::pair{InteractionEdgeKind::Contradicts, "contradicts"sv},
            std::pair{InteractionEdgeKind::Supersedes, "supersedes"sv}, std::pair{InteractionEdgeKind::ClosedBy, "closed_by"sv}};
        constexpr std::array visibility_names{std::pair{InteractionVisibility::User, "user"sv},
                                              std::pair{InteractionVisibility::Operations, "operations"sv}, std::pair{InteractionVisibility::Audit, "audit"sv}};
        constexpr std::array state_names{std::pair{InteractionObjectState::Pending, "pending"sv},
                                         std::pair{InteractionObjectState::Running, "running"sv}, std::pair{InteractionObjectState::Waiting, "waiting"sv},
                                         std::pair{InteractionObjectState::Passed, "passed"sv}, std::pair{InteractionObjectState::Warning, "warning"sv},
                                         std::pair{InteractionObjectState::Blocked, "blocked"sv}, std::pair{InteractionObjectState::Failed, "failed"sv},
                                         std::pair{InteractionObjectState::Superseded, "superseded"sv}, std::pair{InteractionObjectState::Unavailable, "unavailable"sv}};

        void issue(std::vector<contracts::ContractIssue> &out, std::string code, std::string path, std::string message) { out.push_back({std::move(code), std::move(path), std::move(message)}); }
        std::string digest(nlohmann::json value)
        {
            value.erase("digest");
            return contracts::canonical_digest(value).value_or("");
        }
        bool valid_digest(const nlohmann::json &value) { return value.contains("digest") && value.at("digest").is_string() && value.at("digest") == digest(value); }
        bool fields(const nlohmann::json &j, const std::set<std::string> &allowed, std::vector<contracts::ContractIssue> *issues)
        {
            if (!j.is_object())
            {
                contracts::append_issue(issues, "object_required", "/", "object required");
                return false;
            }
            for (const auto &[key, _] : j.items())
                if (!allowed.count(key))
                {
                    contracts::append_issue(issues, "unknown_field", "/" + key, "unknown field");
                    return false;
                }
            return true;
        }
        std::string required_string(const nlohmann::json &j, const char *key)
        {
            if (!j.contains(key) || !j.at(key).is_string())
                throw std::invalid_argument(std::string("string field required: ") + key);
            return j.at(key);
        }
        InteractionSourceRevision source(const nlohmann::json &j) { return {required_string(j, "store"), required_string(j, "object_id"), j.at("revision").get<std::uint64_t>(), required_string(j, "digest")}; }
        InteractionNavigationTarget navigation(const nlohmann::json &j) { return {required_string(j, "view"), required_string(j, "object_id"), j.value("revision", std::uint64_t{0})}; }
    }
    std::string_view name(InteractionNodeKind v) noexcept { return enum_name(v, node_names); }
    std::string_view name(InteractionEdgeKind v) noexcept { return enum_name(v, edge_names); }
    std::string_view name(InteractionVisibility v) noexcept { return enum_name(v, visibility_names); }
    std::string_view name(InteractionObjectState v) noexcept { return enum_name(v, state_names); }
    std::optional<InteractionNodeKind> interaction_node_kind(std::string_view v) noexcept { return parse_enum(v, node_names); }
    std::optional<InteractionEdgeKind> interaction_edge_kind(std::string_view v) noexcept { return parse_enum(v, edge_names); }
    std::optional<InteractionVisibility> interaction_visibility(std::string_view v) noexcept { return parse_enum(v, visibility_names); }
    std::optional<InteractionObjectState> interaction_object_state(std::string_view v) noexcept { return parse_enum(v, state_names); }

    nlohmann::json encode(const InteractionRef &r) { return {{"tenant_id", r.tenant_id}, {"conversation_id", r.conversation_id}, {"turn_id", r.turn_id}, {"message_id", r.message_id}, {"task_id", r.task_id}, {"run_id", r.run_id}, {"harness_id", r.harness_id}, {"decision_id",r.decision_id}, {"plan_id", r.plan_id}, {"plan_node_id", r.plan_node_id}, {"plan_revision", r.plan_revision}, {"agent_template_id", r.agent_template_id}, {"agent_invocation_id", r.agent_invocation_id}, {"child_agent_id", r.child_agent_id}, {"skill_node_id", r.skill_node_id}, {"tool_invocation_id", r.tool_invocation_id}, {"memory_snapshot_id", r.memory_snapshot_id}, {"memory_view_digest", r.memory_view_digest}, {"approval_id", r.approval_id}, {"evidence_id", r.evidence_id}, {"finding_id", r.finding_id}, {"artifact_id", r.artifact_id}, {"object_revision_digest", r.object_revision_digest}}; }
    nlohmann::json encode(const InteractionSourceRevision &s) { return {{"store", s.store}, {"object_id", s.object_id}, {"revision", s.revision}, {"digest", s.digest}}; }
    nlohmann::json encode(const InteractionNode &n)
    {
        nlohmann::json j = {{"schema_version", "agent.ui.interaction_node/v1"}, {"node_id", n.node_id}, {"kind", name(n.kind)}, {"ref", encode(n.ref)}, {"revision", n.revision}, {"label", n.label}, {"summary", n.summary}, {"display", n.display}, {"state", name(n.state)}, {"visibility", name(n.visibility)}, {"source", encode(n.source)}, {"updated_at", n.updated_at}};
        j["digest"] = digest(j);
        return j;
    }
    nlohmann::json encode(const InteractionEdge &e)
    {
        nlohmann::json j = {{"schema_version", "agent.ui.interaction_edge/v1"}, {"edge_id", e.edge_id}, {"kind", name(e.kind)}, {"from_node_id", e.from_node_id}, {"to_node_id", e.to_node_id}, {"revision", e.revision}, {"visibility", name(e.visibility)}, {"source", encode(e.source)}, {"updated_at", e.updated_at}};
        j["digest"] = digest(j);
        return j;
    }
    nlohmann::json encode(const UiInteractionEvent &e)
    {
        nlohmann::json related = nlohmann::json::array();
        for (const auto &r : e.related_refs)
            related.push_back(encode(r));
        nlohmann::json j = {{"schema_version", "agent.ui.interaction_event/v1"}, {"event_id", e.event_id}, {"tenant_id", e.tenant_id}, {"conversation_id", e.conversation_id}, {"sequence", e.sequence}, {"event_type", e.event_type}, {"visibility", name(e.visibility)}, {"primary_ref", encode(e.primary_ref)}, {"related_refs", std::move(related)}, {"display", e.display}, {"navigation_target", {{"view", e.navigation_target.view}, {"object_id", e.navigation_target.object_id}, {"revision", e.navigation_target.revision}}}, {"source", encode(e.source)}, {"timestamp", e.timestamp}};
        j["digest"] = digest(j);
        return j;
    }
    nlohmann::json encode(const InteractionSnapshot &s)
    {
        nlohmann::json ns = nlohmann::json::array(), es = nlohmann::json::array(), sr = nlohmann::json::array();
        for (const auto &n : s.nodes)
            ns.push_back(encode(n));
        for (const auto &e : s.edges)
            es.push_back(encode(e));
        for (const auto &r : s.source_revisions)
            sr.push_back(encode(r));
        nlohmann::json j = {{"schema_version", "agent.ui.interaction_snapshot/v1"}, {"tenant_id", s.tenant_id}, {"conversation_id", s.conversation_id}, {"revision", s.revision}, {"head_sequence", s.head_sequence}, {"nodes", std::move(ns)}, {"edges", std::move(es)}, {"orphan_edge_ids", s.orphan_edge_ids}, {"source_revisions", std::move(sr)}, {"updated_at", s.updated_at}};
        j["digest"] = digest(j);
        return j;
    }

    std::optional<InteractionRef> decode_interaction_ref(const nlohmann::json &j, std::vector<contracts::ContractIssue> *issues)
    {
        static const std::set<std::string> allowed = {"tenant_id", "conversation_id", "turn_id", "message_id", "task_id", "run_id", "harness_id", "decision_id", "plan_id", "plan_node_id", "plan_revision", "agent_template_id", "agent_invocation_id", "child_agent_id", "skill_node_id", "tool_invocation_id", "memory_snapshot_id", "memory_view_digest", "approval_id", "evidence_id", "finding_id", "artifact_id", "object_revision_digest"};
        if (!fields(j, allowed, issues))
            return std::nullopt;
        try
        {
            InteractionRef r;
            r.tenant_id = required_string(j, "tenant_id");
            r.conversation_id = j.value("conversation_id", "");
            r.turn_id = j.value("turn_id", "");
            r.message_id = j.value("message_id", "");
            r.task_id = j.value("task_id", "");
            r.run_id = j.value("run_id", "");
            r.harness_id = j.value("harness_id", "");
            r.decision_id = j.value("decision_id", "");
            r.plan_id = j.value("plan_id", "");
            r.plan_node_id = j.value("plan_node_id", "");
            r.plan_revision = j.value("plan_revision", std::uint64_t{0});
            r.agent_template_id = j.value("agent_template_id", "");
            r.agent_invocation_id = j.value("agent_invocation_id", "");
            r.child_agent_id = j.value("child_agent_id", "");
            r.skill_node_id = j.value("skill_node_id", "");
            r.tool_invocation_id = j.value("tool_invocation_id", "");
            r.memory_snapshot_id = j.value("memory_snapshot_id", "");
            r.memory_view_digest = j.value("memory_view_digest", "");
            r.approval_id = j.value("approval_id", "");
            r.evidence_id = j.value("evidence_id", "");
            r.finding_id = j.value("finding_id", "");
            r.artifact_id = j.value("artifact_id", "");
            r.object_revision_digest = j.value("object_revision_digest", "");
            return r;
        }
        catch (const std::exception &e)
        {
            contracts::append_issue(issues, "interaction_ref_invalid", "/", e.what());
            return std::nullopt;
        }
    }

    std::optional<InteractionNode> decode_interaction_node(const nlohmann::json &j, std::vector<contracts::ContractIssue> *issues)
    {
        static const std::set<std::string> allowed = {"schema_version", "node_id", "kind", "ref", "revision", "label", "summary", "display", "state", "visibility", "source", "updated_at", "digest"};
        if (!fields(j, allowed, issues)) return std::nullopt;
        try
        {
            if (!valid_digest(j))
                throw std::invalid_argument("node digest invalid");
            auto kind = interaction_node_kind(required_string(j, "kind"));
            auto state = interaction_object_state(required_string(j, "state"));
            auto visibility = interaction_visibility(required_string(j, "visibility"));
            auto ref = decode_interaction_ref(j.at("ref"), issues);
            if (!kind || !state || !visibility || !ref)
                throw std::invalid_argument("node enum/ref invalid");
            InteractionNode n;
            n.node_id=required_string(j,"node_id"); n.kind=*kind; n.ref=*ref;
            n.revision=j.at("revision").get<std::uint64_t>(); n.label=required_string(j,"label");
            n.summary=required_string(j,"summary"); n.display=j.at("display"); n.state=*state;
            n.visibility=*visibility; n.source=source(j.at("source"));
            n.updated_at=required_string(j,"updated_at"); n.digest=required_string(j,"digest");
            if (!validate(n).empty())
                throw std::invalid_argument("node validation failed");
            return n;
        }
        catch (const std::exception &e)
        {
            contracts::append_issue(issues, "interaction_node_invalid", "/", e.what());
            return std::nullopt;
        }
    }
    std::optional<InteractionEdge> decode_interaction_edge(const nlohmann::json &j, std::vector<contracts::ContractIssue> *issues)
    {
        static const std::set<std::string> allowed = {"schema_version", "edge_id", "kind", "from_node_id", "to_node_id", "revision", "visibility", "source", "updated_at", "digest"};
        if (!fields(j, allowed, issues)) return std::nullopt;
        try
        {
            if (!valid_digest(j))
                throw std::invalid_argument("edge digest invalid");
            auto kind = interaction_edge_kind(required_string(j, "kind"));
            auto visibility = interaction_visibility(required_string(j, "visibility"));
            if (!kind || !visibility)
                throw std::invalid_argument("edge enum invalid");
            InteractionEdge e{required_string(j, "edge_id"), *kind, required_string(j, "from_node_id"), required_string(j, "to_node_id"), j.at("revision").get<std::uint64_t>(), *visibility, source(j.at("source")), required_string(j, "updated_at"), required_string(j, "digest")};
            if (!validate(e).empty())
                throw std::invalid_argument("edge validation failed");
            return e;
        }
        catch (const std::exception &e)
        {
            contracts::append_issue(issues, "interaction_edge_invalid", "/", e.what());
            return std::nullopt;
        }
    }
    std::optional<UiInteractionEvent> decode_interaction_event(const nlohmann::json &j, std::vector<contracts::ContractIssue> *issues)
    {
        static const std::set<std::string> allowed = {"schema_version", "event_id", "tenant_id", "conversation_id", "sequence", "event_type", "visibility", "primary_ref", "related_refs", "display", "navigation_target", "source", "timestamp", "digest"};
        if (!fields(j, allowed, issues)) return std::nullopt;
        try
        {
            if (!valid_digest(j))
                throw std::invalid_argument("event digest invalid");
            auto visibility = interaction_visibility(required_string(j, "visibility"));
            auto primary = decode_interaction_ref(j.at("primary_ref"), issues);
            if (!visibility || !primary)
                throw std::invalid_argument("event visibility/ref invalid");
            UiInteractionEvent e;
            e.event_id = required_string(j, "event_id");
            e.tenant_id = required_string(j, "tenant_id");
            e.conversation_id = required_string(j, "conversation_id");
            e.sequence = j.at("sequence");
            e.event_type = required_string(j, "event_type");
            e.visibility = *visibility;
            e.primary_ref = *primary;
            for (const auto &value : j.at("related_refs"))
            {
                auto r = decode_interaction_ref(value, issues);
                if (!r)
                    throw std::invalid_argument("related ref invalid");
                e.related_refs.push_back(*r);
            }
            e.display = j.at("display");
            e.navigation_target = navigation(j.at("navigation_target"));
            e.source = source(j.at("source"));
            e.timestamp = required_string(j, "timestamp");
            e.digest = required_string(j, "digest");
            if (!validate(e).empty())
                throw std::invalid_argument("event validation failed");
            return e;
        }
        catch (const std::exception &e)
        {
            contracts::append_issue(issues, "interaction_event_invalid", "/", e.what());
            return std::nullopt;
        }
    }

    std::optional<InteractionSnapshot> decode_interaction_snapshot(const nlohmann::json &j,
                                                                    std::vector<contracts::ContractIssue> *issues)
    {
        static const std::set<std::string> allowed = {"schema_version","tenant_id","conversation_id","revision","head_sequence","nodes","edges","orphan_edge_ids","source_revisions","updated_at","digest"};
        if (!fields(j, allowed, issues)) return std::nullopt;
        try {
            if (!valid_digest(j)) throw std::invalid_argument("snapshot digest invalid");
            InteractionSnapshot s;s.tenant_id=required_string(j,"tenant_id");s.conversation_id=required_string(j,"conversation_id");
            s.revision=j.at("revision").get<std::uint64_t>();s.head_sequence=j.value("head_sequence",std::uint64_t{0});
            s.updated_at=j.value("updated_at","");s.digest=required_string(j,"digest");
            s.orphan_edge_ids=j.value("orphan_edge_ids",std::vector<std::string>{});
            for(const auto& item:j.at("nodes")){auto n=decode_interaction_node(item,issues);if(!n)return std::nullopt;s.nodes.push_back(std::move(*n));}
            for(const auto& item:j.at("edges")){auto e=decode_interaction_edge(item,issues);if(!e)return std::nullopt;s.edges.push_back(std::move(*e));}
            for(const auto& item:j.at("source_revisions"))s.source_revisions.push_back(source(item));
            std::set<std::string> ids;for(const auto& n:s.nodes)if(!ids.insert(n.node_id).second)throw std::invalid_argument("duplicate node id");
            for(const auto& e:s.edges)if(!ids.count(e.from_node_id)||!ids.count(e.to_node_id))throw std::invalid_argument("edge endpoint missing");
            return s;
        } catch(const std::exception& e){contracts::append_issue(issues,"interaction_snapshot_invalid","/",e.what());return std::nullopt;}
    }

    std::vector<contracts::ContractIssue> validate(const InteractionRef &r, std::optional<InteractionNodeKind> kind)
    {
        std::vector<contracts::ContractIssue> out;
        if (r.tenant_id.empty())
            issue(out, "tenant_required", "/tenant_id", "tenant is required");
        auto require = [&](bool ok, const char *field)
        {if(!ok)issue(out,"identity_required",std::string("/")+field,std::string(field)+" is required for node kind"); };
        if (kind)
            switch (*kind)
            {
            case InteractionNodeKind::Message:
                require(!r.conversation_id.empty() && !r.turn_id.empty() && !r.message_id.empty(), "message_id");
                break;
            case InteractionNodeKind::Thinking:
            case InteractionNodeKind::CognitionStage:
                require(!r.conversation_id.empty() && !r.turn_id.empty(), "turn_id");
                break;
            case InteractionNodeKind::Plan:
                require(!r.plan_id.empty() && r.plan_revision > 0, "plan_id");
                break;
            case InteractionNodeKind::PlanNode:
                require(!r.plan_id.empty() && r.plan_revision > 0 && !r.plan_node_id.empty(), "plan_node_id");
                break;
            case InteractionNodeKind::MemoryView:
                require(!r.memory_snapshot_id.empty() && !r.memory_view_digest.empty(), "memory_view_digest");
                break;
            case InteractionNodeKind::Agent:
                require(!r.agent_invocation_id.empty(), "agent_invocation_id");
                break;
            case InteractionNodeKind::SkillNode:
                require(!r.agent_invocation_id.empty() && !r.skill_node_id.empty(), "skill_node_id");
                break;
            case InteractionNodeKind::ToolInvocation:
                require(!r.tool_invocation_id.empty(), "tool_invocation_id");
                break;
            case InteractionNodeKind::Approval:
                require(!r.approval_id.empty(), "approval_id");
                break;
            case InteractionNodeKind::Evidence:
                require(!r.evidence_id.empty(), "evidence_id");
                break;
            case InteractionNodeKind::Finding:
                require(!r.finding_id.empty(), "finding_id");
                break;
            case InteractionNodeKind::Artifact:
                require(!r.artifact_id.empty(), "artifact_id");
                break;
            case InteractionNodeKind::Closure:
                require(!r.task_id.empty() && !r.run_id.empty(), "run_id");
                break;
            }
        return out;
    }
    std::vector<contracts::ContractIssue> validate(const InteractionNode &n)
    {
        auto out = validate(n.ref, n.kind);
        if (n.node_id.empty())
            issue(out, "node_id_required", "/node_id", "node id required");
        if (!n.revision)
            issue(out, "revision_required", "/revision", "positive revision required");
        if (n.source.store.empty() || n.source.object_id.empty() || !n.source.revision || n.source.digest.empty())
            issue(out, "source_revision_required", "/source", "complete source revision required");
        if (n.label.size() > 512 || n.summary.size() > 4096)
            issue(out, "display_too_large", "/summary", "display text exceeds limit");
        if (!n.display.is_object() || contains_forbidden_display_field(n.display) || n.display.dump().size() > 65536)
            issue(out, "unsafe_display", "/display", "display contains forbidden, invalid, or oversized data");
        return out;
    }
    std::vector<contracts::ContractIssue> validate(const InteractionEdge &e)
    {
        std::vector<contracts::ContractIssue> out;
        if (e.edge_id.empty() || e.from_node_id.empty() || e.to_node_id.empty())
            issue(out, "edge_identity_required", "/", "edge/from/to required");
        if (e.from_node_id == e.to_node_id && e.kind != InteractionEdgeKind::Supersedes)
            issue(out, "self_edge_forbidden", "/to_node_id", "self edge forbidden");
        if (!e.revision || e.source.store.empty() || e.source.object_id.empty() || !e.source.revision || e.source.digest.empty())
            issue(out, "source_revision_required", "/source", "complete source revision required");
        return out;
    }
    std::vector<contracts::ContractIssue> validate(const UiInteractionEvent &e)
    {
        std::vector<contracts::ContractIssue> out;
        if (e.event_id.empty() || e.tenant_id.empty() || e.conversation_id.empty() || !e.sequence || e.event_type.empty())
            issue(out, "event_identity_required", "/", "event identity and sequence required");
        if (e.primary_ref.tenant_id != e.tenant_id || (!e.primary_ref.conversation_id.empty() && e.primary_ref.conversation_id != e.conversation_id))
            issue(out, "event_ref_scope_mismatch", "/primary_ref", "primary ref scope differs from event");
        for (const auto &r : e.related_refs)
            if (r.tenant_id != e.tenant_id || (!r.conversation_id.empty() && r.conversation_id != e.conversation_id))
                issue(out, "cross_scope_ref", "/related_refs", "related ref crosses tenant/conversation");
        if (!e.display.is_object() || contains_forbidden_display_field(e.display))
            issue(out, "unsafe_display", "/display", "display contains forbidden or invalid fields");
        if (e.navigation_target.view.empty() || e.navigation_target.object_id.empty())
            issue(out, "navigation_target_required", "/navigation_target", "view and object id required");
        if (e.source.store.empty() || e.source.object_id.empty() || !e.source.revision || e.source.digest.empty())
            issue(out, "source_revision_required", "/source", "complete source revision required");
        return out;
    }
    bool visibility_allows(InteractionVisibility viewer, InteractionVisibility object) noexcept { return static_cast<int>(viewer) >= static_cast<int>(object); }
    bool contains_forbidden_display_field(const nlohmann::json &value) noexcept
    {
        try
        {
            if (value.is_object())
                for (const auto &[key, item] : value.items())
                {
                    std::string lower = key;
                    for (char &c : lower)
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (lower == "chain_of_thought" || lower == "raw_prompt" || lower == "credential" || lower == "secret" || lower == "authorization" || contains_forbidden_display_field(item))
                        return true;
                }
            else if (value.is_array())
                for (const auto &item : value)
                    if (contains_forbidden_display_field(item))
                        return true;
            return false;
        }
        catch (...)
        {
            return true;
        }
    }
} // namespace agent_framework::ui
