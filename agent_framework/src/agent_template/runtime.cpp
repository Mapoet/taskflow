#include "agent/agent_template/runtime.hpp"

#include <algorithm>
#include <atomic>
#include <stdexcept>

#include "agent/contracts/contract.hpp"

namespace agent_framework::agent_template {
namespace {
std::string deterministic_id(std::string_view prefix, const nlohmann::json& seed) {
    const auto digest = contracts::canonical_digest(seed).value_or("digest-unavailable");
    return std::string(prefix) + "-" + digest.substr(digest.find(':') + 1, 20);
}

nlohmann::json values_to_json(const workflow::ValueMap& values) {
    nlohmann::json out = nlohmann::json::object();
    for (const auto& [key, value] : values) {
        if (value.type() == typeid(nlohmann::json)) out[key] = std::any_cast<nlohmann::json>(value);
        else if (value.type() == typeid(std::string)) out[key] = std::any_cast<std::string>(value);
        else if (value.type() == typeid(const char*)) out[key] = std::any_cast<const char*>(value);
        else if (value.type() == typeid(bool)) out[key] = std::any_cast<bool>(value);
        else if (value.type() == typeid(int)) out[key] = std::any_cast<int>(value);
        else if (value.type() == typeid(std::int64_t)) out[key] = std::any_cast<std::int64_t>(value);
        else if (value.type() == typeid(double)) out[key] = std::any_cast<double>(value);
        else throw std::invalid_argument("AgentTemplate adapter input is not JSON-compatible: " + key);
    }
    return out;
}

std::vector<SkillCandidate> candidates_for(const AgentTemplate& agent_template,
                                           const SkillRegistrySnapshot& snapshot,
                                           const nlohmann::json& input) {
    SkillCandidateRetriever retriever;
    std::vector<SkillCandidate> out;
    for (const auto& role : agent_template.roles) {
        CandidateQuery query;
        query.task = input.dump();
        query.required_capabilities = role.selector.required_capabilities;
        query.candidate_ids = role.selector.candidates;
        if (role.selector.skill_id) query.candidate_ids.push_back(*role.selector.skill_id);
        query.role = role.role;
        query.top_k = std::max<std::uint64_t>(role.max_cardinality, 1);
        auto selected = retriever.retrieve(snapshot, query);
        out.insert(out.end(), selected.begin(), selected.end());
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.score != b.score ? a.score > b.score : a.skill_id < b.skill_id;
    });
    out.erase(std::unique(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.skill_id == b.skill_id;
    }), out.end());
    return out;
}
}  // namespace

AgentRuntime::AgentRuntime(std::shared_ptr<AgentTemplateRegistry> templates,
                           std::shared_ptr<SkillRegistry> skills,
                           std::shared_ptr<SkillRunnerRegistry> runners)
    : templates_(std::move(templates)), skills_(std::move(skills)), runners_(std::move(runners)) {}

void AgentRuntime::set_model_plan_provider(std::shared_ptr<SkillPlanProvider> provider) {
    model_provider_ = std::move(provider);
}
void AgentRuntime::set_hybrid_plan_provider(std::shared_ptr<SkillPlanProvider> provider) {
    hybrid_provider_ = std::move(provider);
}
void AgentRuntime::set_completion_authority(std::shared_ptr<AgentCompletionAuthority> authority) {
    completion_authority_ = std::move(authority);
}

std::shared_ptr<SkillPlanProvider> AgentRuntime::provider_for(AgentBusinessMode mode) const {
    switch (mode) {
        case AgentBusinessMode::FixedWorkflow: return std::make_shared<FixedWorkflowPlanProvider>();
        case AgentBusinessMode::DirectiveDriven: return std::make_shared<DirectiveSkillPlanProvider>();
        case AgentBusinessMode::ModelDriven: return model_provider_;
        case AgentBusinessMode::Hybrid: return hybrid_provider_ ? hybrid_provider_ : model_provider_;
    }
    return {};
}

AgentRunResult AgentRuntime::run(const TemplateRef& ref, const nlohmann::json& input,
                                 AgentRunOptions options) const {
    AgentRunResult out;
    if (!templates_ || !skills_ || !runners_) {
        out.error_code = "runtime_dependency_missing";
        return out;
    }
    auto agent_template = templates_->load(options.metadata.identity.tenant_id,
                                           ref.template_id, ref.revision);
    if (!agent_template) {
        out.error_code = "template_not_found";
        return out;
    }
    const auto template_document = encode(*agent_template);
    const auto template_digest = template_document.at("canonical_digest").get<std::string>();
    if (!ref.digest.empty() && ref.digest != template_digest) {
        out.error_code = "template_digest_mismatch";
        return out;
    }
    out.agent_template = *agent_template;
    auto provider = provider_for(agent_template->business_mode);
    if (!provider) {
        out.error_code = "plan_provider_unavailable";
        return out;
    }
    const auto snapshot = skills_->snapshot();
    if (!snapshot.valid()) {
        out.error_code = "skill_registry_unavailable";
        return out;
    }
    auto candidates = candidates_for(*agent_template, snapshot, input);
    PlanningContext context{options.metadata, *agent_template, input,
                            agent_template->permissions, agent_template->budgets, 1};
    SkillCollaborationPlan plan;
    try { plan = provider->propose(context, candidates); }
    catch (const std::exception& error) {
        out.error_code = "planning_failed";
        out.error_message = error.what();
        return out;
    }
    SkillCollaborationPlanValidator validator;
    const auto validation = validator.validate(*agent_template, plan,
                                               agent_template->permissions,
                                               agent_template->budgets, nullptr, 1);
    if (!validation.ok) {
        out.error_code = "plan_rejected";
        out.issues = validation.issues;
        return out;
    }
    out.plan = plan;
    const auto plan_digest = encode(plan).at("canonical_digest").get<std::string>();
    const nlohmann::json identity_seed{{"template", template_digest}, {"plan", plan_digest},
                                       {"input", input}, {"hosting", to_string(options.hosting_mode)}};
    if (options.invocation_id.empty()) options.invocation_id = deterministic_id("inv", identity_seed);
    if (options.session_id.empty()) options.session_id = deterministic_id("session", identity_seed);
    SessionBuildRequest session_request{options.metadata, options.session_id, plan, snapshot,
                                        agent_template->permissions, options.model_profiles_digest,
                                        options.prompt_revisions_digest, options.deployment_generation};
    ActiveSkillSessionBuilder session_builder;
    auto built = session_builder.build(session_request);
    if (!built.session) {
        out.error_code = "session_build_failed";
        out.issues = std::move(built.issues);
        return out;
    }
    out.session = *built.session;
    const auto session_digest = encode(*built.session).at("canonical_digest").get<std::string>();
    AgentTemplateInvocation invocation;
    invocation.metadata = options.metadata;
    invocation.invocation_id = options.invocation_id;
    invocation.template_ref = {agent_template->template_id, agent_template->revision, template_digest};
    invocation.plan_digest = plan_digest;
    invocation.skill_session_digest = session_digest;
    invocation.model_profiles_digest = options.model_profiles_digest;
    invocation.capability_snapshot_digest = options.capability_snapshot_digest.empty()
        ? built.session->capability_snapshot_digest : options.capability_snapshot_digest;
    invocation.permissions = built.session->effective_permissions;
    invocation.budget = built.session->budget;
    invocation.context_projection_ref = options.context_projection_ref;
    invocation.deployment_generation = options.deployment_generation;
    invocation.business_mode = agent_template->business_mode;
    invocation.hosting_mode = options.hosting_mode;
    const auto persisted = templates_->create_invocation(invocation);
    if (!persisted.ok()) {
        out.error_code = "invocation_persist_failed";
        out.error_message = persisted.message;
        return out;
    }
    out.invocation = invocation;
    out.execution = SkillWorkflowCompiler(runners_).execute(plan, invocation, *built.session,
                                                             input, options.cancel);
    if (!out.execution.ok) {
        out.error_code = out.execution.error_code;
        out.error_message = out.execution.error_message;
        return out;
    }
    if (options.require_completion_authority) {
        if (!completion_authority_) {
            out.error_code = "completion_authority_unavailable";
            return out;
        }
        out.completion = completion_authority_->evaluate(
            {invocation, plan, *built.session, out.execution.receipts, out.execution.output});
        if (!out.completion->accepted) {
            out.error_code = "completion_rejected";
            out.error_message = out.completion->reason_code;
            return out;
        }
    }
    out.ok = true;
    return out;
}

std::pair<std::shared_ptr<workflow::AnyNode>, tf::Task> AgentTemplateNode::create(
    workflow::GraphBuilder& builder, const std::string& name,
    const std::vector<std::pair<std::string, std::string>>& input_specs,
    std::shared_ptr<AgentRuntime> runtime, TemplateRef template_ref,
    AgentRunOptions options, const std::string& output_key) {
    if (!runtime) throw std::invalid_argument("AgentTemplateNode requires AgentRuntime");
    options.hosting_mode = AgentHostingMode::WorkflowNode;
    return builder.create_any_node(name, input_specs,
        [runtime = std::move(runtime), template_ref = std::move(template_ref), options,
         output_key](const workflow::ValueMap& values) mutable {
            workflow::ValueMap output;
            output[output_key] = runtime->run(template_ref, values_to_json(values), options);
            return output;
        }, {output_key});
}

AgentRunResult AgentTemplateSubflow::run(std::shared_ptr<AgentRuntime> runtime,
                                         const TemplateRef& template_ref,
                                         const workflow::ValueMap& inputs,
                                         AgentRunOptions options) {
    if (!runtime) throw std::invalid_argument("AgentTemplateSubflow requires AgentRuntime");
    options.hosting_mode = AgentHostingMode::WorkflowSubflow;
    return runtime->run(template_ref, values_to_json(inputs), std::move(options));
}

}  // namespace agent_framework::agent_template
