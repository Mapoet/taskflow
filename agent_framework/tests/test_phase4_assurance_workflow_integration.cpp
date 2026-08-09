#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>

#include <taskflow/taskflow.hpp>

#include "agent/assurance/assurance_graph_template.hpp"
#include "agent/internal/agent_thread_state.hpp"
#include "agent/prompt_renderer/prompt_renderer.hpp"
#include "phase4_assurance_workflow_test_support.hpp"
#include "phase4_llm_runtime_test_support.hpp"

namespace {
using namespace phase4_assurance_test;

std::string profile_id(AssuranceStage stage) {
    return "f4v." + assurance_stage_name(stage);
}

void publish_role(const std::shared_ptr<llm_runtime::LLMRuntimeStore>& store,
                  const contracts::ContractMetadata& metadata, AssuranceStage stage,
                  std::string candidate_id, std::string provider, std::string model,
                  std::string group) {
    llm_runtime::LLMRoleProfile profile;
    profile.metadata = metadata;
    profile.profile_id = profile_id(stage);
    profile.revision = "r1";
    profile.role = profile.profile_id;
    profile.provider_pool = {candidate_id};
    profile.reasoning_effort = llm_runtime::ReasoningEffort::High;
    profile.max_context_tokens = 16384;
    profile.max_output_tokens = 4096;
    profile.prompt_id = profile.profile_id + ".prompt";
    profile.prompt_revision = "r1";
    profile.memory_view_profile = "verification";
    profile.required_capabilities = {"repo_read"};
    profile.allowed_regions = {"local"};
    profile.independence_group = std::move(group);
    profile.evidence_authority = llm_runtime::EvidenceAuthority::Advisory;
    profile.timeout_ms = 5000;
    profile.max_attempts = 1;
    profile.max_fallbacks = 0;
    profile.calibration_revision = profile.profile_id + ".cal";

    llm_runtime::PromptRevision prompt;
    prompt.metadata = metadata;
    prompt.prompt_id = profile.prompt_id;
    prompt.revision = "r1";
    prompt.system_template = "Perform an independent read-only professional verification role.";
    prompt.user_template = "{{input}}";
    prompt.input_schema = {{"type", "object"},
                           {"properties", {{"input", {{"type", "string"}}}}},
                           {"required", {"input"}}, {"additionalProperties", false}};
    prompt.output_schema = {{"type", "object"}, {"additionalProperties", true}};
    prompt.structured_output_required = true;
    prompt.max_repair_attempts = 0;
    prompt.compatibility_class = "f4v-r1";

    llm_runtime::RoleCalibrationRecord calibration;
    calibration.metadata = metadata;
    calibration.calibration_id = profile.calibration_revision;
    calibration.role = profile.role;
    calibration.profile_id = profile.profile_id;
    calibration.profile_revision = profile.revision;
    calibration.prompt_revision = prompt.revision;
    calibration.provider = std::move(provider);
    calibration.model = std::move(model);
    calibration.dataset_revision = "f4v-offline-fixture-r1";
    calibration.metrics = {{"schema_success", 1.0}};
    calibration.thresholds = {{"schema_success", 0.99}};
    calibration.approved = true;
    calibration.decision_id = "offline-fixture";
    assert(store->publish_profile(profile).ok());
    assert(store->publish_prompt(prompt).ok());
    assert(store->publish_calibration(calibration).ok());
}

void push_remaining_after_architecture(ScriptedAssuranceModel& model) {
    int index = 2;
    for(const auto stage : {AssuranceStage::ArchitectureVerification,
                            AssuranceStage::DomainVerification,
                            AssuranceStage::SecurityVerification,
                            AssuranceStage::CompletenessVerification}) {
        const auto label = std::to_string(index++);
        model.push(stage, verifier_output(stage), "provider-" + label,
                   "model-" + label, "group-" + label);
    }
    model.push(AssuranceStage::EvidenceResolution, resolver_output(),
               "resolver-provider", "resolver-model", "resolver-group");
}
}

int main() {
    using namespace phase4_assurance_test;
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto base = std::filesystem::temp_directory_path() /
                      ("taskflow-phase4-f4v-integration-" + suffix);

    // Production semantic adapter: all seven LLM stages route through pinned RoleRuntime roles.
    {
        using phase4_llm_test::ScriptedAdapter;
        using phase4_llm_test::output;
        auto value = contract("task-f4v-role-runtime");
        auto runtime_store = std::make_shared<llm_runtime::InMemoryLLMRuntimeStore>();
        auto router = std::make_shared<llm_runtime::ModelRouter>();
        auto client = std::make_shared<LLMClient>();
        client->set_prompt_renderer(std::make_shared<PromptRenderer>());
        const std::vector<AssuranceStage> stages = {
            AssuranceStage::Planning, AssuranceStage::CodeVerification,
            AssuranceStage::ArchitectureVerification, AssuranceStage::DomainVerification,
            AssuranceStage::SecurityVerification, AssuranceStage::CompletenessVerification,
            AssuranceStage::EvidenceResolution};
        std::vector<std::shared_ptr<ScriptedAdapter>> adapters;
        for(std::size_t index = 0; index < stages.size(); ++index) {
            const auto stage = stages[index];
            const auto label = std::to_string(index + 1);
            const auto candidate_id = "candidate-" + label;
            const auto provider = "fake-f4v-" + label;
            const auto model_name = "f4v-model-" + label;
            auto candidate = phase4_llm_test::candidate(candidate_id, provider, model_name, 10);
            candidate.independence_group = "candidate-group-" + label;
            assert(router->register_candidate(candidate));
            publish_role(runtime_store, value.metadata, stage, candidate_id, provider,
                         model_name, "role-group-" + label);
            auto adapter = std::make_shared<ScriptedAdapter>();
            const auto response = stage == AssuranceStage::Planning ? planner_output() :
                stage == AssuranceStage::EvidenceResolution ? resolver_output() :
                verifier_output(stage);
            adapter->push([response] { return output(response.dump()); });
            client->register_adapter(provider, adapter);
            adapters.push_back(std::move(adapter));
        }
        auto runtime = std::make_shared<llm_runtime::RoleRuntime>(
            client, runtime_store, router, nullptr, nullptr,
            llm_runtime::RoleRuntimeOptions{true, true,
                [] { return "2026-08-10T00:00:00Z"; }, {}});
        RoleRuntimeAssuranceModel stage_model(runtime);
        for(const auto stage : stages)
            assert(stage_model.bind(stage, {profile_id(stage), "r1", {"repo_read"}, "local"}));
        memory_v2::MemoryProviderRegistry providers;
        memory_v2::MemoryViewEngine views(providers);
        OracleRegistry oracles;
        register_manifest_oracle(oracles);
        InMemoryAssuranceStore assurance_store;
        ProfessionalAssuranceWorkflow workflow(views, oracles, assurance_store, stage_model);
        AssuranceWorkflowOptions options;
        options.workflow_id = "workflow-f4v-role-runtime";
        options.max_stage_attempts = 1;
        options.now = [] { return "2026-08-10T00:00:00Z"; };
        const auto result = workflow.run(value, subject(value.metadata), task_context(),
                                         manifest(value), options);
        assert(result.report && result.report->decision == AcceptanceDecision::Accepted);
        for(const auto& adapter : adapters) assert(adapter->calls() == 1);
        for(const auto stage : stages) {
            const auto invocation = runtime_store->load_invocation(
                "tenant-a", options.workflow_id + ":" + assurance_stage_name(stage) + ":1");
            assert(invocation && invocation->manifest.profile_id == profile_id(stage));
            assert(invocation->manifest.memory_view_profile == "verification");
        }
    }

    // Process death after the committed attempt boundary resumes without rerunning prior roles.
    const auto recovery_path = base.string() + "-recovery.sqlite";
    auto recovery_value = contract("task-f4v-recovery");
    {
        memory_v2::MemoryProviderRegistry providers;
        memory_v2::MemoryViewEngine views(providers);
        OracleRegistry oracles;
        register_manifest_oracle(oracles);
        SQLiteAssuranceStore store(recovery_path);
        ScriptedAssuranceModel model;
        model.push(AssuranceStage::Planning, planner_output(),
                   "planner-provider", "planner-model", "planner-group");
        model.push(AssuranceStage::CodeVerification,
                   verifier_output(AssuranceStage::CodeVerification),
                   "code-provider", "code-model", "code-group");
        model.interrupt_once(AssuranceStage::ArchitectureVerification);
        ProfessionalAssuranceWorkflow workflow(views, oracles, store, model);
        AssuranceWorkflowOptions options;
        options.workflow_id = "workflow-f4v-recovery";
        options.max_stage_attempts = 1;
        options.now = [] { return "2026-08-10T00:00:00Z"; };
        bool terminated = false;
        try {
            (void)workflow.run(recovery_value, subject(recovery_value.metadata), task_context(),
                               manifest(recovery_value), options);
        } catch(const std::runtime_error&) {
            terminated = true;
        }
        assert(terminated);
        const auto stored = store.load_checkpoint("tenant-a", options.workflow_id);
        assert(stored && stored->checkpoint.next_stage == AssuranceStage::ArchitectureVerification);
        assert(stored->checkpoint.stage_attempts.at("architecture_verification") == 1);
        assert(std::find(stored->checkpoint.completed_stages.begin(),
                         stored->checkpoint.completed_stages.end(), "code_verification") !=
               stored->checkpoint.completed_stages.end());
    }
    {
        memory_v2::MemoryProviderRegistry providers;
        memory_v2::MemoryViewEngine views(providers);
        OracleRegistry oracles;
        register_manifest_oracle(oracles);
        SQLiteAssuranceStore store(recovery_path);
        ScriptedAssuranceModel model;
        push_remaining_after_architecture(model);
        ProfessionalAssuranceWorkflow workflow(views, oracles, store, model);
        AssuranceWorkflowOptions options;
        options.workflow_id = "workflow-f4v-recovery";
        options.max_stage_attempts = 1;
        options.now = [] { return "2026-08-10T00:00:00Z"; };
        const auto result = workflow.run(recovery_value, subject(recovery_value.metadata),
                                         task_context(), manifest(recovery_value), options);
        assert(result.report && result.report->decision == AcceptanceDecision::Accepted);
        assert(std::none_of(model.requests.begin(), model.requests.end(), [](const auto& request) {
            return request.stage == AssuranceStage::Planning ||
                   request.stage == AssuranceStage::CodeVerification;
        }));
        const auto architecture = std::find_if(model.requests.begin(), model.requests.end(),
            [](const auto& request) { return request.stage == AssuranceStage::ArchitectureVerification; });
        assert(architecture != model.requests.end() && architecture->attempt == 2);
    }

    // GraphExecutor uses the same durable workflow and emits common execution events.
    {
        auto value = contract("task-f4v-graph");
        memory_v2::MemoryProviderRegistry providers;
        memory_v2::MemoryViewEngine views(providers);
        OracleRegistry oracles;
        register_manifest_oracle(oracles);
        InMemoryAssuranceStore store;
        ScriptedAssuranceModel model;
        script_success(model);
        auto workflow = std::make_shared<ProfessionalAssuranceWorkflow>(
            views, oracles, store, model);
        AssuranceWorkflowOptions options;
        options.workflow_id = "workflow-f4v-graph";
        options.max_stage_attempts = 1;
        options.now = [] { return "2026-08-10T00:00:00Z"; };
        auto graph_template = std::make_shared<AssuranceGraphTemplate>(
            workflow, value, subject(value.metadata), task_context(), manifest(value), options);
        GraphExecutor graph;
        graph.register_template(graph_template->get_template_name(), graph_template);
        tf::Executor executor;
        ExecutionRequest request;
        request.template_id = graph_template->get_template_name();
        request.session = std::make_shared<internal::AgentThreadState>();
        request.session->initial_user_prompt = "Verify all deliverables professionally.";
        request.context.session_id = "session-f4v-graph";
        request.context.task_id = value.metadata.identity.task_id;
        request.context.tenant_id = value.metadata.identity.tenant_id;
        request.options.input_already_processed = true;
        request.options.persist_session = false;
        std::vector<ExecutionEvent> events;
        request.event_sink = [&](const auto& event) { events.push_back(event); };
        const auto executed = graph.execute_sync(executor, request);
        assert(executed.success);
        assert(executed.outputs.at("state") == "completed");
        assert(executed.outputs.at("acceptance_report").at("payload").at("decision") == "accepted");
        assert(std::any_of(events.begin(), events.end(), [](const auto& event) {
            return event.payload.value("component", "") == "professional_assurance";
        }));
    }

    std::filesystem::remove(recovery_path);
    std::filesystem::remove(recovery_path + "-wal");
    std::filesystem::remove(recovery_path + "-shm");
    return 0;
}
