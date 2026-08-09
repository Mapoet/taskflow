#include <cassert>
#include <chrono>
#include <filesystem>

#include "agent/prompt_renderer/prompt_renderer.hpp"
#include "phase4_remediation_test_support.hpp"
#include "phase4_llm_runtime_test_support.hpp"

namespace {
using namespace phase4_remediation_test;

std::string profile_id(RemediationStage stage) {
    return "f5r." + remediation_stage_name(stage);
}
void publish_role(const std::shared_ptr<llm_runtime::LLMRuntimeStore>& store,
                  const contracts::ContractMetadata& metadata, RemediationStage stage,
                  std::string candidate_id, std::string provider, std::string model) {
    llm_runtime::LLMRoleProfile profile; profile.metadata = metadata;
    profile.profile_id = profile_id(stage); profile.revision = "r1"; profile.role = profile.profile_id;
    profile.provider_pool = {candidate_id}; profile.reasoning_effort = llm_runtime::ReasoningEffort::High;
    profile.max_context_tokens = 16384; profile.max_output_tokens = 4096;
    profile.prompt_id = profile.profile_id + ".prompt"; profile.prompt_revision = "r1";
    profile.memory_view_profile = "replan"; profile.required_capabilities = {"repo_read"};
    profile.allowed_regions = {"local"}; profile.independence_group = profile.profile_id + ".group";
    profile.evidence_authority = llm_runtime::EvidenceAuthority::Advisory;
    profile.timeout_ms = 5000; profile.max_attempts = 1; profile.max_fallbacks = 0;
    profile.calibration_revision = profile.profile_id + ".cal";
    llm_runtime::PromptRevision prompt; prompt.metadata = metadata; prompt.prompt_id = profile.prompt_id;
    prompt.revision = "r1"; prompt.system_template = "Perform a bounded remediation workflow role.";
    prompt.user_template = "{{input}}";
    prompt.input_schema = {{"type", "object"}, {"properties", {{"input", {{"type", "string"}}}}},
                           {"required", {"input"}}, {"additionalProperties", false}};
    prompt.output_schema = {{"type", "object"}, {"additionalProperties", true}};
    prompt.structured_output_required = true; prompt.max_repair_attempts = 0;
    prompt.compatibility_class = "f5r-r1";
    llm_runtime::RoleCalibrationRecord calibration; calibration.metadata = metadata;
    calibration.calibration_id = profile.calibration_revision; calibration.role = profile.role;
    calibration.profile_id = profile.profile_id; calibration.profile_revision = profile.revision;
    calibration.prompt_revision = prompt.revision; calibration.provider = std::move(provider);
    calibration.model = std::move(model); calibration.dataset_revision = "f5r-offline-r1";
    calibration.metrics = {{"schema_success", 1.0}}; calibration.thresholds = {{"schema_success", 0.99}};
    calibration.approved = true; calibration.decision_id = "offline-fixture";
    assert(store->publish_profile(profile).ok()); assert(store->publish_prompt(prompt).ok());
    assert(store->publish_calibration(calibration).ok());
}
}

int main() {
    using namespace phase4_remediation_test;
    {
        using phase4_llm_test::ScriptedAdapter;
        using phase4_llm_test::output;
        auto rp = execution_plan("task-f5r-role-runtime"); auto rc = contract(rp); auto rr = report(rp, rc);
        auto ra = assurance_checkpoint(rp, rc, rr); auto ri = inventory(rp, rr);
        auto runtime_store = std::make_shared<llm_runtime::InMemoryLLMRuntimeStore>();
        auto router = std::make_shared<llm_runtime::ModelRouter>();
        auto client = std::make_shared<LLMClient>();
        client->set_prompt_renderer(std::make_shared<PromptRenderer>());
        const std::vector<RemediationStage> stages = {RemediationStage::ImpactAnalysis,
            RemediationStage::RemediationPlanning, RemediationStage::ReverificationPlanning};
        std::vector<std::shared_ptr<ScriptedAdapter>> adapters;
        for(std::size_t n = 0; n < stages.size(); ++n) {
            const auto stage = stages[n]; const auto label = std::to_string(n + 1);
            const auto candidate_id = "f5r-candidate-" + label;
            const auto provider = "fake-f5r-" + label; const auto model_name = "f5r-model-" + label;
            auto candidate = phase4_llm_test::candidate(candidate_id, provider, model_name, 10);
            candidate.independence_group = "f5r-candidate-group-" + label;
            assert(router->register_candidate(candidate));
            publish_role(runtime_store, rp.metadata, stage, candidate_id, provider, model_name);
            auto adapter = std::make_shared<ScriptedAdapter>();
            const auto response = stage == RemediationStage::ImpactAnalysis ? impact_output() :
                stage == RemediationStage::RemediationPlanning ? planner_output() : reverify_output();
            adapter->push([response] { return output(response.dump()); });
            client->register_adapter(provider, adapter); adapters.push_back(std::move(adapter));
        }
        auto runtime = std::make_shared<llm_runtime::RoleRuntime>(client, runtime_store, router,
            nullptr, nullptr, llm_runtime::RoleRuntimeOptions{true, true,
                [] { return "2026-08-10T00:00:00Z"; }, {}});
        RoleRuntimeRemediationModel stage_model(runtime);
        for(const auto stage : stages)
            assert(stage_model.bind(stage, {profile_id(stage), "r1", {"repo_read"}, "local"}));
        memory_v2::MemoryProviderRegistry providers; memory_v2::MemoryViewEngine views(providers);
        InMemoryRemediationStore store; planning::InMemoryPlanStore plans; assert(plans.create(rp));
        LLMRemediationWorkflow workflow(views, store, plans, stage_model);
        auto ro = options("role-runtime-f5r");
        const auto result = workflow.run(rp, rc, rr, ra, ri, subject(rp.metadata), ro);
        assert(result.state == RemediationState::ReadyForExecution);
        for(const auto& adapter : adapters) assert(adapter->calls() == 1);
        for(const auto stage : stages) {
            const auto invocation = runtime_store->load_invocation(
                "tenant-a", ro.workflow_id + ":" + remediation_stage_name(stage) + ":1");
            assert(invocation && invocation->manifest.profile_id == profile_id(stage));
            assert(invocation->manifest.memory_view_profile == "replan");
        }
    }
    auto p = execution_plan("task-f5r-restart"); auto c = contract(p); auto r = report(p, c);
    auto a = assurance_checkpoint(p, c, r); auto i = inventory(p, r);
    memory_v2::MemoryProviderRegistry providers; memory_v2::MemoryViewEngine views(providers);
    planning::InMemoryPlanStore plans; assert(plans.create(p));
    ScriptedModel model; script_success(model);
    const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto path = (std::filesystem::temp_directory_path() /
                       ("taskflow-f5r-restart-" + suffix + ".sqlite")).string();
    bool interrupted = false;
    {
        SQLiteRemediationStore store(path); LLMRemediationWorkflow workflow(views, store, plans, model);
        auto o = options("restart-f5r"); o.after_plan_commit = [&] {
            if(!interrupted) { interrupted = true; throw std::runtime_error("simulated process death"); }
        };
        const auto first = workflow.run(p, c, r, a, i, subject(p.metadata), o);
        assert(first.state == RemediationState::Failed && first.error_code == "remediation_interrupted");
        const auto committed = plans.current(p.metadata.identity);
        assert(committed && committed->plan_revision == 2);
    }
    {
        SQLiteRemediationStore reopened(path); LLMRemediationWorkflow workflow(views, reopened, plans, model);
        const auto resumed = workflow.run(p, c, r, a, i, subject(p.metadata), options("restart-f5r"));
        assert(resumed.state == RemediationState::ReadyForExecution);
        assert(resumed.checkpoint.committed_plan_digest ==
               planning::encode(*resumed.proposed_plan).at("canonical_digest").get<std::string>());
        assert(plans.history(p.metadata.identity).size() == 2);
        assert(model.requests.size() == 3);
    }
    std::filesystem::remove(path); std::filesystem::remove(path + "-wal"); std::filesystem::remove(path + "-shm");
    return 0;
}
