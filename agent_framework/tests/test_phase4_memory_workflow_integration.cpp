#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>

#include <taskflow/taskflow.hpp>

#include "agent/internal/agent_thread_state.hpp"
#include "agent/memory_v2/workflows/memory_graph_template.hpp"
#include "agent/prompt_renderer/prompt_renderer.hpp"
#include "phase4_llm_runtime_test_support.hpp"
#include "phase4_memory_workflow_test_support.hpp"

namespace {
using namespace agent_framework;
using namespace agent_framework::memory_v2::workflows;

std::string profile_id(MemoryWorkflowStage stage) {
    return "f3m." + memory_workflow_stage_name(stage);
}

void publish_role(const std::shared_ptr<llm_runtime::LLMRuntimeStore>& store,
                  const contracts::ContractMetadata& metadata,
                  MemoryWorkflowStage stage, std::string candidate,
                  std::string provider, std::string model, std::string group) {
    llm_runtime::LLMRoleProfile profile;
    profile.metadata = metadata;
    profile.profile_id = profile_id(stage);
    profile.revision = "r1";
    profile.role = profile.profile_id;
    profile.provider_pool = {std::move(candidate)};
    profile.reasoning_effort = llm_runtime::ReasoningEffort::High;
    profile.max_context_tokens = 16384;
    profile.max_output_tokens = 4096;
    profile.prompt_id = profile.profile_id + ".prompt";
    profile.prompt_revision = "r1";
    switch(stage) {
        case MemoryWorkflowStage::Extraction:
        case MemoryWorkflowStage::Normalization:
        case MemoryWorkflowStage::Consolidation:
        case MemoryWorkflowStage::ConflictResolution:
        case MemoryWorkflowStage::TaskStateUpdate:
            profile.memory_view_profile = "memory-consolidation";
            break;
        case MemoryWorkflowStage::QueryPlanning:
        case MemoryWorkflowStage::Reranking:
            profile.memory_view_profile = "memory-retrieval";
            break;
        case MemoryWorkflowStage::DynamicView:
            profile.memory_view_profile = "memory-view-policy";
            break;
        case MemoryWorkflowStage::GovernanceRecommendation:
            profile.memory_view_profile = "memory-governance";
            break;
        case MemoryWorkflowStage::Complete:
            profile.memory_view_profile = "memory-complete";
            break;
    }
    profile.required_capabilities = {"repo_read"};
    profile.allowed_regions = {"local"};
    profile.independence_group = std::move(group);
    profile.evidence_authority = llm_runtime::EvidenceAuthority::Candidate;
    profile.timeout_ms = 5000;
    profile.max_attempts = 1;
    profile.max_fallbacks = 0;
    profile.calibration_revision = profile.profile_id + ".cal";

    llm_runtime::PromptRevision prompt;
    prompt.metadata = metadata;
    prompt.prompt_id = profile.prompt_id;
    prompt.revision = "r1";
    prompt.system_template = "Produce a candidate or recommendation; never mutate authority.";
    prompt.user_template = "{{input}}";
    prompt.input_schema = {{"type", "object"},
                           {"properties", {{"input", {{"type", "string"}}}}},
                           {"required", {"input"}}, {"additionalProperties", false}};
    prompt.output_schema = {{"type", "object"}, {"additionalProperties", true}};
    prompt.structured_output_required = true;
    prompt.max_repair_attempts = 0;
    prompt.compatibility_class = "f3m-r1";

    llm_runtime::RoleCalibrationRecord calibration;
    calibration.metadata = metadata;
    calibration.calibration_id = profile.calibration_revision;
    calibration.role = profile.role;
    calibration.profile_id = profile.profile_id;
    calibration.profile_revision = profile.revision;
    calibration.prompt_revision = prompt.revision;
    calibration.provider = std::move(provider);
    calibration.model = std::move(model);
    calibration.dataset_revision = "f3m-fixture-r1";
    calibration.metrics = {{"schema_success", 1.0}};
    calibration.thresholds = {{"schema_success", 0.99}};
    calibration.approved = true;
    calibration.decision_id = "offline-fixture";
    assert(store->publish_profile(profile).ok());
    assert(store->publish_prompt(prompt).ok());
    assert(store->publish_calibration(calibration).ok());
}
}  // namespace

int main() {
    using namespace phase4_memory_workflow_test;
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto base = std::filesystem::temp_directory_path() /
                      ("taskflow-phase4-f3m-recovery-" + suffix);
    const auto checkpoint_path = base.string() + "-checkpoint.sqlite";
    const auto memory_path = base.string() + "-memory.sqlite";
    const auto value = input("workflow-recovery");

    // Production adapter path: all nine semantic stages are pinned RoleRuntime invocations.
    {
        using phase4_llm_test::ScriptedAdapter;
        using phase4_llm_test::output;
        auto runtime_input = input("workflow-role-runtime");
        auto llm_store = std::make_shared<llm_runtime::InMemoryLLMRuntimeStore>();
        auto router = std::make_shared<llm_runtime::ModelRouter>();
        auto primary = phase4_llm_test::candidate(
            "f3m-primary", "fake-memory", "memory-model", 10);
        auto query = phase4_llm_test::candidate(
            "f3m-query", "fake-query", "query-model", 10);
        auto rerank = phase4_llm_test::candidate(
            "f3m-rerank", "fake-rerank", "rerank-model", 10);
        auto governance = phase4_llm_test::candidate(
            "f3m-governance", "fake-governance", "governance-model", 10);
        assert(router->register_candidate(primary));
        assert(router->register_candidate(query));
        assert(router->register_candidate(rerank));
        assert(router->register_candidate(governance));
        for(const auto stage : {MemoryWorkflowStage::Extraction,
                               MemoryWorkflowStage::Normalization,
                               MemoryWorkflowStage::Consolidation,
                               MemoryWorkflowStage::ConflictResolution,
                               MemoryWorkflowStage::TaskStateUpdate,
                               MemoryWorkflowStage::DynamicView})
            publish_role(llm_store, runtime_input.metadata, stage, "f3m-primary",
                         "fake-memory", "memory-model", "primary");
        publish_role(llm_store, runtime_input.metadata, MemoryWorkflowStage::QueryPlanning,
                     "f3m-query", "fake-query", "query-model", "query");
        publish_role(llm_store, runtime_input.metadata, MemoryWorkflowStage::Reranking,
                     "f3m-rerank", "fake-rerank", "rerank-model", "rerank");
        publish_role(llm_store, runtime_input.metadata,
                     MemoryWorkflowStage::GovernanceRecommendation,
                     "f3m-governance", "fake-governance", "governance-model", "governance");

        auto primary_adapter = std::make_shared<ScriptedAdapter>();
        primary_adapter->push([&] { return output(extraction_output().dump()); });
        primary_adapter->push([&] { return output(normalization_output().dump()); });
        primary_adapter->push([&] { return output(consolidation_output(runtime_input).dump()); });
        primary_adapter->push([&] { return output(conflict_output().dump()); });
        primary_adapter->push([&] { return output(task_state_output().dump()); });
        primary_adapter->push([&] { return output(dynamic_view_output(runtime_input).dump()); });
        auto query_adapter = std::make_shared<ScriptedAdapter>();
        query_adapter->push([&] { return output(query_plan_output().dump()); });
        auto rerank_adapter = std::make_shared<ScriptedAdapter>();
        rerank_adapter->push([&] { return output(reranking_output(runtime_input).dump()); });
        auto governance_adapter = std::make_shared<ScriptedAdapter>();
        governance_adapter->push([&] { return output(governance_output(runtime_input).dump()); });
        auto client = std::make_shared<LLMClient>();
        client->set_prompt_renderer(std::make_shared<PromptRenderer>());
        client->register_adapter("fake-memory", primary_adapter);
        client->register_adapter("fake-query", query_adapter);
        client->register_adapter("fake-rerank", rerank_adapter);
        client->register_adapter("fake-governance", governance_adapter);
        auto runtime = std::make_shared<llm_runtime::RoleRuntime>(client, llm_store, router);
        RoleRuntimeMemoryModel stage_model(runtime);
        for(int index = 0; index < static_cast<int>(MemoryWorkflowStage::Complete); ++index) {
            const auto stage = static_cast<MemoryWorkflowStage>(index);
            assert(stage_model.bind(stage, {profile_id(stage), "r1", {"repo_read"}, "local"}));
        }

        const auto runtime_memory_path = base.string() + "-runtime-memory.sqlite";
        auto store = std::make_shared<SQLiteMemoryStore>(runtime_memory_path);
        assert(store->append(existing_record(runtime_input.metadata)));
        MemoryProviderRegistry providers;
        assert(providers.register_provider(std::make_shared<StoreMemoryProvider>("store", store)));
        MemoryViewEngine views(providers);
        InMemoryMemoryWorkflowCheckpointStore checkpoints;
        MultiLayerMemoryWorkflow workflow(views, store, checkpoints, stage_model);
        const auto result = workflow.run(runtime_input);
        assert(result.state == MemoryWorkflowState::AwaitingApproval);
        assert(primary_adapter->calls() == 6);
        assert(query_adapter->calls() == 1);
        assert(rerank_adapter->calls() == 1);
        assert(governance_adapter->calls() == 1);
        const auto rerank_invocation = llm_store->load_invocation(
            "tenant-a", runtime_input.workflow_id + ":reranking:1");
        assert(rerank_invocation && rerank_invocation->manifest.provider == "fake-rerank");
        const auto governance_invocation = llm_store->load_invocation(
            "tenant-a", runtime_input.workflow_id + ":governance_recommendation:1");
        assert(governance_invocation &&
               governance_invocation->manifest.provider == "fake-governance");
        store.reset();
        std::filesystem::remove(runtime_memory_path);
        std::filesystem::remove(runtime_memory_path + "-wal");
        std::filesystem::remove(runtime_memory_path + "-shm");
    }

    // Process death after the durable attempt boundary resumes without repeating completed stages.
    {
        auto store = std::make_shared<SQLiteMemoryStore>(memory_path);
        assert(store->append(existing_record(value.metadata)));
        MemoryProviderRegistry providers;
        assert(providers.register_provider(std::make_shared<StoreMemoryProvider>("store", store)));
        MemoryViewEngine views(providers);
        SQLiteMemoryWorkflowCheckpointStore checkpoints(checkpoint_path);
        ScriptedMemoryModel model;
        model.push(MemoryWorkflowStage::Extraction, extraction_output());
        model.push(MemoryWorkflowStage::Normalization, normalization_output());
        model.push(MemoryWorkflowStage::Consolidation, consolidation_output(value));
        model.interrupt_once(MemoryWorkflowStage::ConflictResolution);
        MultiLayerMemoryWorkflow workflow(views, store, checkpoints, model);
        bool terminated = false;
        try { (void)workflow.run(value); }
        catch(const std::runtime_error&) { terminated = true; }
        assert(terminated);
        const auto durable = checkpoints.load("tenant-a", value.workflow_id);
        assert(durable && durable->checkpoint.state == MemoryWorkflowState::Running);
        assert(durable->checkpoint.next_stage == MemoryWorkflowStage::ConflictResolution);
        assert(durable->checkpoint.stage_attempts.at("conflict_resolution") == 1);
        assert(durable->checkpoint.candidate_record_ids.size() == 1);
    }
    {
        auto store = std::make_shared<SQLiteMemoryStore>(memory_path);
        MemoryProviderRegistry providers;
        assert(providers.register_provider(std::make_shared<StoreMemoryProvider>("store", store)));
        MemoryViewEngine views(providers);
        SQLiteMemoryWorkflowCheckpointStore checkpoints(checkpoint_path);
        ScriptedMemoryModel model;
        model.push(MemoryWorkflowStage::ConflictResolution, conflict_output());
        model.push(MemoryWorkflowStage::TaskStateUpdate, task_state_output());
        model.push(MemoryWorkflowStage::QueryPlanning, query_plan_output(),
                   "query-provider", "query-model", "query");
        model.push(MemoryWorkflowStage::Reranking, reranking_output(value),
                   "rerank-provider", "rerank-model", "rerank");
        model.push(MemoryWorkflowStage::DynamicView, dynamic_view_output(value));
        model.push(MemoryWorkflowStage::GovernanceRecommendation, governance_output(value, false),
                   "governance-provider", "governance-model", "governance");
        MultiLayerMemoryWorkflow workflow(views, store, checkpoints, model);
        const auto recovered = workflow.run(value);
        assert(recovered.state == MemoryWorkflowState::Completed);
        const auto conflict = std::find_if(model.requests.begin(), model.requests.end(),
            [](const auto& request) {
                return request.stage == MemoryWorkflowStage::ConflictResolution;
            });
        assert(conflict != model.requests.end() && conflict->attempt == 2);
        assert(std::none_of(model.requests.begin(), model.requests.end(), [](const auto& request) {
            return request.stage == MemoryWorkflowStage::Extraction ||
                   request.stage == MemoryWorkflowStage::Normalization ||
                   request.stage == MemoryWorkflowStage::Consolidation;
        }));
    }

    // GraphExecutor carries the memory workflow through its common execution envelope.
    {
        auto graph_input = input("workflow-graph");
        auto store = std::make_shared<SQLiteMemoryStore>(memory_path);
        MemoryProviderRegistry providers;
        assert(providers.register_provider(std::make_shared<StoreMemoryProvider>("store", store)));
        MemoryViewEngine views(providers);
        InMemoryMemoryWorkflowCheckpointStore checkpoints;
        ScriptedMemoryModel model;
        script_success(model, graph_input, false);
        auto workflow = std::make_shared<MultiLayerMemoryWorkflow>(
            views, store, checkpoints, model);
        auto graph_template = std::make_shared<MemoryWorkflowGraphTemplate>(
            workflow, graph_input);
        GraphExecutor graph;
        graph.register_template(graph_template->get_template_name(), graph_template);
        tf::Executor executor;
        ExecutionRequest request;
        request.template_id = graph_template->get_template_name();
        request.session = std::make_shared<internal::AgentThreadState>();
        request.session->initial_user_prompt = "Preserve compatibility and retain evidence.";
        request.context.session_id = "session-memory-graph";
        request.context.task_id = graph_input.metadata.identity.task_id;
        request.context.tenant_id = graph_input.metadata.identity.tenant_id;
        request.options.input_already_processed = true;
        request.options.persist_session = false;
        std::vector<ExecutionEvent> events;
        request.event_sink = [&](const auto& event) { events.push_back(event); };
        const auto executed = graph.execute_sync(executor, request);
        assert(executed.success);
        assert(executed.outputs.at("state") == "completed");
        assert(std::any_of(events.begin(), events.end(), [](const auto& event) {
            return event.payload.value("component", "") == "memory_workflow";
        }));
    }

    for(const auto& path : {checkpoint_path, memory_path}) {
        std::filesystem::remove(path);
        std::filesystem::remove(path + "-wal");
        std::filesystem::remove(path + "-shm");
    }
    return 0;
}
