#include <cassert>
#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>

#include <taskflow/taskflow.hpp>

#include "agent/internal/agent_thread_state.hpp"
#include "agent/planning/cognition_graph_template.hpp"
#include "agent/prompt_renderer/prompt_renderer.hpp"
#include "phase4_cognition_pipeline_test_support.hpp"
#include "phase4_llm_runtime_test_support.hpp"

namespace {
using namespace agent_framework;
using namespace agent_framework::planning;
using namespace phase4_cognition_test;

std::string stage_profile(CognitionStage stage) {
    return "f2c." + cognition_stage_name(stage);
}

void publish_role(const std::shared_ptr<llm_runtime::LLMRuntimeStore>& store,
                  const TaskIntake& task, CognitionStage stage,
                  std::string candidate, std::string provider, std::string model,
                  std::string group) {
    llm_runtime::LLMRoleProfile profile;
    profile.metadata = task.metadata;
    profile.profile_id = stage_profile(stage);
    profile.revision = "r1";
    profile.role = profile.profile_id;
    profile.provider_pool = {std::move(candidate)};
    profile.reasoning_effort = llm_runtime::ReasoningEffort::High;
    profile.temperature = 0.0;
    profile.max_context_tokens = 8192;
    profile.max_output_tokens = 4096;
    profile.prompt_id = profile.profile_id + ".prompt";
    profile.prompt_revision = "r1";
    switch(stage) {
        case CognitionStage::Intake: profile.memory_view_profile = "intake"; break;
        case CognitionStage::Strategy:
        case CognitionStage::Synthesis: profile.memory_view_profile = "investigation"; break;
        case CognitionStage::Revision: profile.memory_view_profile = "replan"; break;
        default: profile.memory_view_profile = "planning"; break;
    }
    profile.required_capabilities = {"repo_read"};
    profile.allowed_regions = {"local"};
    profile.independence_group = std::move(group);
    profile.evidence_authority = llm_runtime::EvidenceAuthority::Advisory;
    profile.timeout_ms = 5000;
    profile.max_attempts = 1;
    profile.max_fallbacks = 0;
    profile.calibration_revision = profile.profile_id + ".cal";

    llm_runtime::PromptRevision prompt;
    prompt.metadata = task.metadata;
    prompt.prompt_id = profile.prompt_id;
    prompt.revision = "r1";
    prompt.system_template = "Execute the pinned cognition role.";
    prompt.user_template = "{{input}}";
    prompt.input_schema = {{"type", "object"},
                           {"properties", {{"input", {{"type", "string"}}}}},
                           {"required", {"input"}}, {"additionalProperties", false}};
    prompt.output_schema = {{"type", "object"}, {"additionalProperties", true}};
    prompt.structured_output_required = true;
    prompt.max_repair_attempts = 0;
    prompt.compatibility_class = "f2c-r1";

    llm_runtime::RoleCalibrationRecord calibration;
    calibration.metadata = task.metadata;
    calibration.calibration_id = profile.calibration_revision;
    calibration.role = profile.role;
    calibration.profile_id = profile.profile_id;
    calibration.profile_revision = profile.revision;
    calibration.prompt_revision = prompt.revision;
    calibration.provider = std::move(provider);
    calibration.model = std::move(model);
    calibration.dataset_revision = "f2c-fixture-r1";
    calibration.metrics = {{"schema_success", 1.0}};
    calibration.thresholds = {{"schema_success", 0.99}};
    calibration.approved = true;
    calibration.decision_id = "offline-fixture";
    assert(store->publish_profile(profile).ok());
    assert(store->publish_prompt(prompt).ok());
    assert(store->publish_calibration(calibration).ok());
}

class CrashAtSynthesis final : public CognitionStageModel {
public:
    explicit CrashAtSynthesis(ScriptedStageModel& delegate) : delegate_(delegate) {}
    CognitionStageResponse invoke(const CognitionStageRequest& request) override {
        if(request.stage == CognitionStage::Synthesis)
            throw std::runtime_error("simulated process termination");
        return delegate_.invoke(request);
    }
private:
    ScriptedStageModel& delegate_;
};
}

int main() {
    using namespace agent_framework;
    using namespace agent_framework::planning;
    using namespace phase4_cognition_test;
    using phase4_llm_test::ScriptedAdapter;
    using phase4_llm_test::output;

    // Production adapter path: every semantic stage is routed through RoleRuntime.
    {
        const auto task = intake("task-role-runtime");
        auto llm_store = std::make_shared<llm_runtime::InMemoryLLMRuntimeStore>();
        auto router = std::make_shared<llm_runtime::ModelRouter>();
        auto planner_candidate = phase4_llm_test::candidate(
            "f2c-planner", "fake-planner", "planner-model", 10);
        auto critic_candidate = phase4_llm_test::candidate(
            "f2c-critic", "fake-critic", "critic-model", 10);
        assert(router->register_candidate(planner_candidate));
        assert(router->register_candidate(critic_candidate));
        for(const auto stage : {CognitionStage::Intake, CognitionStage::Strategy,
                               CognitionStage::Synthesis, CognitionStage::Boundary,
                               CognitionStage::Planning})
            publish_role(llm_store, task, stage, "f2c-planner", "fake-planner",
                         "planner-model", "planner");
        publish_role(llm_store, task, CognitionStage::Critique, "f2c-critic",
                     "fake-critic", "critic-model", "critic");

        auto planner_adapter = std::make_shared<ScriptedAdapter>();
        planner_adapter->push([] { return output(intake_output().dump()); });
        planner_adapter->push([] { return output(strategy_output().dump()); });
        planner_adapter->push([] { return output(synthesis_output().dump()); });
        planner_adapter->push([] { return output(boundary_output().dump()); });
        planner_adapter->push([] { return output(plan_output().dump()); });
        auto critic_adapter = std::make_shared<ScriptedAdapter>();
        critic_adapter->push([] { return output(critique_output(true).dump()); });
        auto client = std::make_shared<LLMClient>();
        client->set_prompt_renderer(std::make_shared<PromptRenderer>());
        client->register_adapter("fake-planner", planner_adapter);
        client->register_adapter("fake-critic", critic_adapter);
        auto runtime = std::make_shared<llm_runtime::RoleRuntime>(client, llm_store, router);
        RoleRuntimeCognitionModel stage_model(runtime);
        for(const auto stage : {CognitionStage::Intake, CognitionStage::Strategy,
                               CognitionStage::Synthesis, CognitionStage::Boundary,
                               CognitionStage::Planning, CognitionStage::Critique})
            assert(stage_model.bind(stage,
                {stage_profile(stage), "r1", {"repo_read"}, "local"}));

        memory_v2::MemoryProviderRegistry providers;
        memory_v2::MemoryViewEngine views(providers);
        InvestigatorRegistry investigators;
        assert(investigators.register_investigator(std::make_shared<RepoInvestigator>()));
        assert(investigators.register_investigator(std::make_shared<DocsInvestigator>()));
        InMemoryEvidenceStore evidence;
        InMemoryPlanStore plans;
        InMemoryCognitionCheckpointStore checkpoints;
        MultiStageCognitionWorkflow workflow(
            views, investigators, evidence, plans, checkpoints, stage_model);
        CognitionPipelineOptions options;
        options.pipeline_id = "pipeline-role-runtime";
        const auto result = workflow.run(task, subject(task), options);
        assert(result.state == CognitionPipelineState::Approved);
        assert(planner_adapter->calls() == 5);
        assert(critic_adapter->calls() == 1);
        const auto critic_invocation = llm_store->load_invocation(
            "tenant-a", "pipeline-role-runtime:critique:0:1");
        assert(critic_invocation && critic_invocation->manifest.provider == "fake-critic");
        assert(critic_invocation->manifest.independence_group == "critic");
    }

    // A process dying between stage-attempt checkpoint and result commit resumes from the
    // last durable stage. Evidence and plan state use the durable planning store.
    {
        const auto suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const auto base = std::filesystem::temp_directory_path() /
                          ("taskflow-phase4-f2c-recovery-" + suffix);
        const auto checkpoint_path = base.string() + "-checkpoint.sqlite";
        const auto planning_path = base.string() + "-planning.sqlite";
        const auto task = intake("task-recovery");
        {
            SQLiteCognitionCheckpointStore checkpoints(checkpoint_path);
            SQLitePlanningStore planning(planning_path);
            memory_v2::MemoryProviderRegistry providers;
            memory_v2::MemoryViewEngine views(providers);
            InvestigatorRegistry investigators;
            assert(investigators.register_investigator(std::make_shared<RepoInvestigator>()));
            assert(investigators.register_investigator(std::make_shared<DocsInvestigator>()));
            ScriptedStageModel scripted;
            scripted.push(CognitionStage::Intake, intake_output());
            scripted.push(CognitionStage::Strategy, strategy_output());
            CrashAtSynthesis crash(scripted);
            MultiStageCognitionWorkflow workflow(
                views, investigators, planning, planning, checkpoints, crash);
            CognitionPipelineOptions options;
            options.pipeline_id = "pipeline-recovery";
            bool terminated = false;
            try { (void)workflow.run(task, subject(task), options); }
            catch(const std::runtime_error&) { terminated = true; }
            assert(terminated);
            const auto durable = checkpoints.load("tenant-a", "pipeline-recovery");
            assert(durable && durable->checkpoint.state == CognitionPipelineState::Running);
            assert(durable->checkpoint.next_stage == CognitionStage::Synthesis);
            assert(durable->checkpoint.stage_attempts.at("synthesis:0") == 1);
        }
        {
            SQLiteCognitionCheckpointStore checkpoints(checkpoint_path);
            SQLitePlanningStore planning(planning_path);
            memory_v2::MemoryProviderRegistry providers;
            memory_v2::MemoryViewEngine views(providers);
            InvestigatorRegistry investigators;
            ScriptedStageModel scripted;
            scripted.push(CognitionStage::Synthesis, synthesis_output());
            scripted.push(CognitionStage::Boundary, boundary_output());
            scripted.push(CognitionStage::Planning, plan_output());
            scripted.push(CognitionStage::Critique, critique_output(true),
                          "critic-provider", "critic-model", "critic");
            MultiStageCognitionWorkflow workflow(
                views, investigators, planning, planning, checkpoints, scripted);
            CognitionPipelineOptions options;
            options.pipeline_id = "pipeline-recovery";
            const auto recovered = workflow.run(task, subject(task), options);
            assert(recovered.state == CognitionPipelineState::Approved);
            const auto synthesis = std::find_if(
                scripted.requests.begin(), scripted.requests.end(), [](const auto& request) {
                    return request.stage == CognitionStage::Synthesis;
                });
            assert(synthesis != scripted.requests.end() && synthesis->attempt == 2);
        }
        for(const auto& path : {checkpoint_path, planning_path}) {
            std::filesystem::remove(path);
            std::filesystem::remove(path + "-wal");
            std::filesystem::remove(path + "-shm");
        }
    }

    // GraphExecutor executes the cognition template through its common envelope and exposes
    // stage events and typed outputs. This fixture uses a deterministic model, not a provider.
    {
        const auto task = intake("task-graph");
        auto providers = std::make_shared<memory_v2::MemoryProviderRegistry>();
        auto views = std::make_shared<memory_v2::MemoryViewEngine>(*providers);
        auto investigators = std::make_shared<InvestigatorRegistry>();
        assert(investigators->register_investigator(std::make_shared<RepoInvestigator>()));
        assert(investigators->register_investigator(std::make_shared<DocsInvestigator>()));
        auto evidence = std::make_shared<InMemoryEvidenceStore>();
        auto plans = std::make_shared<InMemoryPlanStore>();
        auto checkpoints = std::make_shared<InMemoryCognitionCheckpointStore>();
        auto model = std::make_shared<ScriptedStageModel>();
        script_success(*model, false);
        auto workflow = std::make_shared<MultiStageCognitionWorkflow>(
            *views, *investigators, *evidence, *plans, *checkpoints, *model);
        CognitionPipelineOptions options;
        options.pipeline_id = "pipeline-graph";
        auto graph_template = std::make_shared<CognitionGraphTemplate>(
            workflow, task, subject(task), options);
        GraphExecutor graph;
        graph.register_template(graph_template->get_template_name(), graph_template);
        tf::Executor executor;
        ExecutionRequest request;
        request.template_id = graph_template->get_template_name();
        request.session = std::make_shared<internal::AgentThreadState>();
        request.session->initial_user_prompt = task.user_goal;
        request.context.session_id = "session-graph";
        request.context.task_id = task.metadata.identity.task_id;
        request.context.tenant_id = task.metadata.identity.tenant_id;
        request.options.input_already_processed = true;
        request.options.persist_session = false;
        std::vector<ExecutionEvent> events;
        request.event_sink = [&](const auto& event) { events.push_back(event); };
        const auto executed = graph.execute_sync(executor, request);
        assert(executed.success);
        assert(executed.outputs.at("state") == "approved");
        assert(executed.outputs.contains("plan"));
        assert(std::any_of(events.begin(), events.end(), [](const auto& event) {
            return event.payload.value("component", "") == "cognition";
        }));
    }
    return 0;
}
