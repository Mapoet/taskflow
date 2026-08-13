#include "agent/agent_template/types.hpp"

#include <array>
#include <set>
#include <stdexcept>
#include <utility>

namespace agent_framework::agent_template
{
    namespace
    {
        using json = nlohmann::json;

        template <typename E, std::size_t N>
        std::string enum_name(E value, const std::array<const char *, N> &names)
        {
            const auto index = static_cast<std::size_t>(value);
            if (index >= N)
                throw std::invalid_argument("enum value out of range");
            return names[index];
        }
        template <typename E, std::size_t N>
        std::optional<E> enum_value(std::string_view value, const std::array<const char *, N> &names)
        {
            for (std::size_t index = 0; index < N; ++index)
                if (value == names[index])
                    return static_cast<E>(index);
            return std::nullopt;
        }
        constexpr std::array business_names = {"model_driven", "directive_driven", "hybrid", "fixed_workflow"};
        constexpr std::array hosting_names = {"standalone", "conversation", "workflow_node", "workflow_subflow", "remote_a2a"};
        constexpr std::array role_names = {"coordinator", "worker", "synthesizer", "verifier", "judge", "approver"};
        constexpr std::array runner_names = {"inline_prompt", "local_capability", "sandboxed_process", "cli", "mcp", "child_agent", "nested_workflow", "human_approval"};
        constexpr std::array effect_names = {"read_only", "write", "unknown"};
        constexpr std::array session_names = {"prepared", "active", "suspended", "completed", "failed", "cancelled"};
        constexpr std::array runner_state_names = {"admitted", "prepared", "running", "attached", "waiting", "checkpointed", "cancelling", "reconciling", "succeeded", "failed", "cancelled"};

        json permission(const PermissionEnvelope &v) { return {{"tools", v.tools}, {"network", v.network}, {"filesystem_read", v.filesystem_read}, {"filesystem_write", v.filesystem_write}, {"environment", v.environment}, {"secrets", v.secrets}}; }
        PermissionEnvelope permission(const json &v)
        {
            PermissionEnvelope out;
            out.tools = v.at("tools").get<std::vector<std::string>>();
            out.network = v.at("network").get<std::vector<std::string>>();
            out.filesystem_read = v.at("filesystem_read").get<std::vector<std::string>>();
            out.filesystem_write = v.at("filesystem_write").get<std::vector<std::string>>();
            out.environment = v.at("environment").get<std::vector<std::string>>();
            out.secrets = v.at("secrets").get<std::vector<std::string>>();
            return out;
        }
        json budget(const BudgetPolicy &v) { return {{"max_iterations", v.max_iterations}, {"max_tool_calls", v.max_tool_calls}, {"max_parallelism", v.max_parallelism}, {"max_tokens", v.max_tokens}, {"deadline_ms", v.deadline_ms}, {"max_cost", v.max_cost}}; }
        BudgetPolicy budget(const json &v)
        {
            BudgetPolicy o;
            o.max_iterations = v.at("max_iterations");
            o.max_tool_calls = v.at("max_tool_calls");
            o.max_parallelism = v.at("max_parallelism");
            o.max_tokens = v.at("max_tokens");
            o.deadline_ms = v.at("deadline_ms");
            o.max_cost = v.at("max_cost");
            return o;
        }
        json selector(const SkillSelector &v)
        {
            json o = {{"required_capabilities", v.required_capabilities}, {"candidates", v.candidates}, {"choose_by_model", v.choose_by_model}};
            if (v.skill_id)
                o["skill_id"] = *v.skill_id;
            return o;
        }
        SkillSelector selector(const json &v)
        {
            SkillSelector o;
            if (v.contains("skill_id"))
                o.skill_id = v.at("skill_id").get<std::string>();
            o.required_capabilities = v.at("required_capabilities").get<std::vector<std::string>>();
            o.candidates = v.at("candidates").get<std::vector<std::string>>();
            o.choose_by_model = v.at("choose_by_model");
            return o;
        }
        json output(const OutputContract &v) { return {{"schema", v.schema}, {"artifact_required", v.artifact_required}, {"evidence_required", v.evidence_required}}; }
        OutputContract output(const json &v) { return {v.at("schema"), v.at("artifact_required"), v.at("evidence_required")}; }
        json node(const SkillPlanNode &v)
        {
            json o = {{"node_id", v.node_id}, {"role", to_string(v.role)}, {"selector", selector(v.selector)}, {"resolved_skill_version", v.resolved_skill_version}, {"resolved_skill_digest", v.resolved_skill_digest}, {"runner", to_string(v.runner)}, {"input_mapping", v.input_mapping}, {"output", output(v.output)}, {"requested_permissions", permission(v.requested_permissions)}, {"effect", to_string(v.effect)}, {"max_attempts", v.max_attempts}, {"failure_policy", v.failure_policy}, {"idempotency_key", v.idempotency_key}, {"required", v.required}, {"verifier", v.verifier}, {"approval_required", v.approval_required}, {"model_replannable", v.model_replannable}};
            if (v.resolved_skill_id)
                o["resolved_skill_id"] = *v.resolved_skill_id;
            return o;
        }
        SkillPlanNode node(const json &v)
        {
            SkillPlanNode o;
            o.node_id = v.at("node_id");
            o.role = *skill_role_from_string(v.at("role").get<std::string>());
            o.selector = selector(v.at("selector"));
            if (v.contains("resolved_skill_id"))
                o.resolved_skill_id = v.at("resolved_skill_id").get<std::string>();
            o.resolved_skill_version = v.at("resolved_skill_version");
            o.resolved_skill_digest = v.at("resolved_skill_digest");
            o.runner = *skill_runner_kind_from_string(v.at("runner").get<std::string>());
            o.input_mapping = v.at("input_mapping");
            o.output = output(v.at("output"));
            o.requested_permissions = permission(v.at("requested_permissions"));
            o.effect = *effect_class_from_string(v.at("effect").get<std::string>());
            o.max_attempts = v.at("max_attempts");
            o.failure_policy = v.at("failure_policy");
            o.idempotency_key = v.at("idempotency_key");
            o.required = v.at("required");
            o.verifier = v.at("verifier");
            o.approval_required = v.at("approval_required");
            o.model_replannable = v.at("model_replannable");
            return o;
        }
        json edge(const SkillPlanEdge &v) { return {{"from", v.from}, {"to", v.to}, {"condition", v.condition}}; }
        SkillPlanEdge edge(const json &v) { return {v.at("from"), v.at("to"), v.at("condition")}; }
        json plan_payload(const SkillCollaborationPlan &v)
        {
            json ns = json::array(), es = json::array();
            for (const auto &x : v.nodes)
                ns.push_back(node(x));
            for (const auto &x : v.edges)
                es.push_back(edge(x));
            return {{"plan_id", v.plan_id}, {"revision", v.revision}, {"parent_digest", v.parent_digest}, {"nodes", std::move(ns)}, {"edges", std::move(es)}, {"budget", budget(v.budget)}, {"output_assembly", v.output_assembly}, {"committed_effect_receipts", v.committed_effect_receipts}};
        }
        SkillCollaborationPlan plan(const contracts::TypedContractDocument &d)
        {
            SkillCollaborationPlan o;
            o.metadata = d.metadata;
            const auto &v = d.payload;
            o.plan_id = v.at("plan_id");
            o.revision = v.at("revision");
            o.parent_digest = v.at("parent_digest");
            for (const auto &x : v.at("nodes"))
                o.nodes.push_back(node(x));
            for (const auto &x : v.at("edges"))
                o.edges.push_back(edge(x));
            o.budget = budget(v.at("budget"));
            o.output_assembly = v.at("output_assembly");
            o.committed_effect_receipts = v.at("committed_effect_receipts").get<std::vector<std::string>>();
            return o;
        }
        json role(const AgentRoleDefinition &v) { return {{"role_id", v.role_id}, {"role", to_string(v.role)}, {"selector", selector(v.selector)}, {"min_cardinality", v.min_cardinality}, {"max_cardinality", v.max_cardinality}, {"required", v.required}}; }
        AgentRoleDefinition role(const json &v) { return {v.at("role_id"), *skill_role_from_string(v.at("role").get<std::string>()), selector(v.at("selector")), v.at("min_cardinality"), v.at("max_cardinality"), v.at("required")}; }
        json pinned(const PinnedSkill &v) { return {{"skill_id", v.skill_id}, {"version", v.version}, {"package_digest", v.package_digest}, {"dependency_lock", v.dependency_lock}, {"effective_permissions", permission(v.effective_permissions)}, {"capability_snapshot_digest", v.capability_snapshot_digest}}; }
        PinnedSkill pinned(const json &v) { return {v.at("skill_id"), v.at("version"), v.at("package_digest"), v.at("dependency_lock").get<std::map<std::string, std::string>>(), permission(v.at("effective_permissions")), v.at("capability_snapshot_digest")}; }
        json typed_ref(const TypedRef &v) { return {{"id", v.id}, {"kind", v.kind}, {"digest", v.digest}, {"uri", v.uri}}; }

        template <typename T, typename Decode>
        std::optional<T> parse(std::string_view kind, const json &value, const contracts::ParseContext &context, std::vector<contracts::ContractIssue> *issues, const std::set<std::string> &fields, Decode decode)
        {
            auto d = contracts::parse_typed_contract(value, kind, context, issues);
            if (!d || !contracts::validate_object_fields(d->payload, fields, fields, contracts::UnknownFieldPolicy::Reject, nullptr, issues, "/payload"))
                return std::nullopt;
            try
            {
                return decode(*d);
            }
            catch (const std::exception &e)
            {
                contracts::append_issue(issues, "payload_decode_failed", "/payload", e.what());
                return std::nullopt;
            }
        }
    } // namespace

    std::string to_string(AgentBusinessMode v) { return enum_name(v, business_names); }
    std::string to_string(AgentHostingMode v) { return enum_name(v, hosting_names); }
    std::string to_string(SkillRole v) { return enum_name(v, role_names); }
    std::string to_string(SkillRunnerKind v) { return enum_name(v, runner_names); }
    std::string to_string(EffectClass v) { return enum_name(v, effect_names); }
    std::string to_string(SessionState v) { return enum_name(v, session_names); }
    std::string to_string(RunnerLifecycleState v) { return enum_name(v, runner_state_names); }
    std::optional<AgentBusinessMode> agent_business_mode_from_string(std::string_view v) { return enum_value<AgentBusinessMode>(v, business_names); }
    std::optional<AgentHostingMode> agent_hosting_mode_from_string(std::string_view v) { return enum_value<AgentHostingMode>(v, hosting_names); }
    std::optional<SkillRole> skill_role_from_string(std::string_view v) { return enum_value<SkillRole>(v, role_names); }
    std::optional<SkillRunnerKind> skill_runner_kind_from_string(std::string_view v) { return enum_value<SkillRunnerKind>(v, runner_names); }
    std::optional<EffectClass> effect_class_from_string(std::string_view v) { return enum_value<EffectClass>(v, effect_names); }
    std::optional<SessionState> session_state_from_string(std::string_view v) { return enum_value<SessionState>(v, session_names); }
    std::optional<RunnerLifecycleState> runner_state_from_string(std::string_view v) { return enum_value<RunnerLifecycleState>(v, runner_state_names); }

    json encode(const SkillCollaborationPlan &v) { return contracts::make_typed_contract(v.metadata, kCollaborationPlanKind, plan_payload(v)); }
    json encode(const AgentTemplate &v)
    {
        json rs = json::array();
        for (const auto &x : v.roles)
            rs.push_back(role(x));
        json p = {{"template_id", v.template_id}, {"revision", v.revision}, {"name", v.name}, {"business_mode", to_string(v.business_mode)}, {"input_schema", v.input_schema}, {"output_schema", v.output_schema}, {"permissions", permission(v.permissions)}, {"budgets", budget(v.budgets)}, {"roles", std::move(rs)}, {"workflow_skeleton", v.workflow_skeleton ? plan_payload(*v.workflow_skeleton) : json(nullptr)}, {"planning_policy", v.planning_policy}, {"recovery_policy", v.recovery_policy}, {"approval_policy", v.approval_policy}, {"assurance_policy", v.assurance_policy}, {"completion_contract", v.completion_contract}};
        return contracts::make_typed_contract(v.metadata, kAgentTemplateKind, std::move(p));
    }
    json encode(const ActiveSkillSession &v)
    {
        json ss = json::array();
        for (const auto &x : v.skills)
            ss.push_back(pinned(x));
        return contracts::make_typed_contract(v.metadata, kActiveSkillSessionKind, {{"session_id", v.session_id}, {"revision", v.revision}, {"state", to_string(v.state)}, {"plan_digest", v.plan_digest}, {"registry_generation", v.registry_generation}, {"skills", std::move(ss)}, {"effective_permissions", permission(v.effective_permissions)}, {"budget", budget(v.budget)}, {"model_profiles_digest", v.model_profiles_digest}, {"prompt_revisions_digest", v.prompt_revisions_digest}, {"capability_snapshot_digest", v.capability_snapshot_digest}, {"deployment_generation", v.deployment_generation}, {"checkpoint_ref", v.checkpoint_ref}});
    }
    json encode(const AgentTemplateInvocation &v) { return contracts::make_typed_contract(v.metadata, kTemplateInvocationKind, {{"invocation_id", v.invocation_id}, {"revision", v.revision}, {"template_ref", {{"template_id", v.template_ref.template_id}, {"revision", v.template_ref.revision}, {"digest", v.template_ref.digest}}}, {"plan_digest", v.plan_digest}, {"skill_session_digest", v.skill_session_digest}, {"model_profiles_digest", v.model_profiles_digest}, {"capability_snapshot_digest", v.capability_snapshot_digest}, {"permissions", permission(v.permissions)}, {"budget", budget(v.budget)}, {"context_projection_ref", v.context_projection_ref}, {"deployment_generation", v.deployment_generation}, {"business_mode", to_string(v.business_mode)}, {"hosting_mode", to_string(v.hosting_mode)}}); }
    json encode(const SkillRunnerReceipt &v)
    {
        json as = json::array(), es = json::array(), rs = json::array();
        for (const auto &x : v.artifacts)
            as.push_back(typed_ref(x));
        for (const auto &x : v.evidence)
            es.push_back(typed_ref(x));
        for (const auto &x : v.effects)
            rs.push_back(typed_ref(x));
        return {{"invocation_id", v.invocation_id}, {"node_id", v.node_id}, {"runner", to_string(v.runner)}, {"terminal_state", to_string(v.terminal_state)}, {"artifacts", std::move(as)}, {"evidence", std::move(es)}, {"effects", std::move(rs)}, {"checkpoint_ref", v.checkpoint_ref}, {"output_digest", v.output_digest}};
    }

    std::optional<SkillCollaborationPlan> decode_collaboration_plan(const json &v, const contracts::ParseContext &c, std::vector<contracts::ContractIssue> *i)
    {
        static const std::set<std::string> f = {"plan_id", "revision", "parent_digest", "nodes", "edges", "budget", "output_assembly", "committed_effect_receipts"};
        return parse<SkillCollaborationPlan>(kCollaborationPlanKind, v, c, i, f, [](const auto &d)
                                             {auto o=plan(d);if(o.plan_id.empty()||o.revision==0)throw std::invalid_argument("plan identity and positive revision required");return o; });
    }
    std::optional<AgentTemplate> decode_agent_template(const json &v, const contracts::ParseContext &c, std::vector<contracts::ContractIssue> *i)
    {
        static const std::set<std::string> f = {"template_id", "revision", "name", "business_mode", "input_schema", "output_schema", "permissions", "budgets", "roles", "workflow_skeleton", "planning_policy", "recovery_policy", "approval_policy", "assurance_policy", "completion_contract"};
        return parse<AgentTemplate>(kAgentTemplateKind, v, c, i, f, [](const auto &d)
                                    {AgentTemplate o;o.metadata=d.metadata;const auto&p=d.payload;o.template_id=p.at("template_id");o.revision=p.at("revision");o.name=p.at("name");o.business_mode=*agent_business_mode_from_string(std::string(p.at("business_mode")));o.input_schema=p.at("input_schema");o.output_schema=p.at("output_schema");o.permissions=permission(p.at("permissions"));o.budgets=budget(p.at("budgets"));for(const auto&x:p.at("roles"))o.roles.push_back(role(x));if(!p.at("workflow_skeleton").is_null()){o.workflow_skeleton=plan({o.metadata,"",p.at("workflow_skeleton")});}o.planning_policy=p.at("planning_policy");o.recovery_policy=p.at("recovery_policy");o.approval_policy=p.at("approval_policy");o.assurance_policy=p.at("assurance_policy");o.completion_contract=p.at("completion_contract");if(o.template_id.empty()||o.revision==0)throw std::invalid_argument("template identity and positive revision required");return o; });
    }
    std::optional<ActiveSkillSession> decode_active_skill_session(const json &v, const contracts::ParseContext &c, std::vector<contracts::ContractIssue> *i)
    {
        static const std::set<std::string> f = {"session_id", "revision", "state", "plan_digest", "registry_generation", "skills", "effective_permissions", "budget", "model_profiles_digest", "prompt_revisions_digest", "capability_snapshot_digest", "deployment_generation", "checkpoint_ref"};
        return parse<ActiveSkillSession>(kActiveSkillSessionKind, v, c, i, f, [](const auto &d)
                                         {ActiveSkillSession o;o.metadata=d.metadata;const auto&p=d.payload;o.session_id=p.at("session_id");o.revision=p.at("revision");o.state=*session_state_from_string(std::string(p.at("state")));o.plan_digest=p.at("plan_digest");o.registry_generation=p.at("registry_generation");for(const auto&x:p.at("skills"))o.skills.push_back(pinned(x));o.effective_permissions=permission(p.at("effective_permissions"));o.budget=budget(p.at("budget"));o.model_profiles_digest=p.at("model_profiles_digest");o.prompt_revisions_digest=p.at("prompt_revisions_digest");o.capability_snapshot_digest=p.at("capability_snapshot_digest");o.deployment_generation=p.at("deployment_generation");o.checkpoint_ref=p.at("checkpoint_ref");if(o.session_id.empty()||o.revision==0)throw std::invalid_argument("session identity and positive revision required");return o; });
    }
    std::optional<AgentTemplateInvocation> decode_template_invocation(const json &v, const contracts::ParseContext &c, std::vector<contracts::ContractIssue> *i)
    {
        static const std::set<std::string> f = {"invocation_id", "revision", "template_ref", "plan_digest", "skill_session_digest", "model_profiles_digest", "capability_snapshot_digest", "permissions", "budget", "context_projection_ref", "deployment_generation", "business_mode", "hosting_mode"};
        return parse<AgentTemplateInvocation>(kTemplateInvocationKind, v, c, i, f, [](const auto &d)
                                              {AgentTemplateInvocation o;o.metadata=d.metadata;const auto&p=d.payload;o.invocation_id=p.at("invocation_id");o.revision=p.at("revision");const auto&t=p.at("template_ref");o.template_ref={t.at("template_id"),t.at("revision"),t.at("digest")};o.plan_digest=p.at("plan_digest");o.skill_session_digest=p.at("skill_session_digest");o.model_profiles_digest=p.at("model_profiles_digest");o.capability_snapshot_digest=p.at("capability_snapshot_digest");o.permissions=permission(p.at("permissions"));o.budget=budget(p.at("budget"));o.context_projection_ref=p.at("context_projection_ref");o.deployment_generation=p.at("deployment_generation");o.business_mode=*agent_business_mode_from_string(std::string(p.at("business_mode")));o.hosting_mode=*agent_hosting_mode_from_string(std::string(p.at("hosting_mode")));if(o.invocation_id.empty()||o.revision==0)throw std::invalid_argument("invocation identity and positive revision required");return o; });
    }
} // namespace agent_framework::agent_template
