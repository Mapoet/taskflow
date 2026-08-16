#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <memory>

#include "agent/harness/production_workflow_adapters.hpp"
#include "phase4_harness_test_support.hpp"

namespace {
using namespace agent_framework;
class EmptyRepository final : public harness::ProductionWorkflowInputRepository {
public:
    std::optional<planning::TaskIntake> intake_value;
    std::optional<planning::TaskIntake> intake(const contracts::ContractIdentity&) override {
        return intake_value;
    }
    std::optional<memory_v2::workflows::MemoryWorkflowInput> memory_input(
        const contracts::ContractIdentity&, std::string_view) override { return {}; }
    std::optional<assurance::AcceptanceContract> acceptance_contract(
        const contracts::ContractIdentity&, std::string_view) override { return {}; }
    std::optional<nlohmann::json> task_context(
        const contracts::ContractIdentity&, std::string_view) override { return {}; }
    std::optional<nlohmann::json> artifact_manifest(
        const contracts::ContractIdentity&, std::string_view) override { return {}; }
    std::optional<assurance::AcceptanceReport> acceptance_report(
        const contracts::ContractIdentity&, std::string_view) override { return {}; }
    std::optional<assurance::AssuranceCheckpoint> assurance_checkpoint(
        const contracts::ContractIdentity&, std::string_view) override { return {}; }
    std::optional<remediation::ImpactInventory> impact_inventory(
        const contracts::ContractIdentity&, std::string_view) override { return {}; }
    std::optional<harness::JudgeWorkflowInput> evaluation_input(
        const contracts::ContractIdentity&) override { return {}; }
};
}

int main() {
    using namespace agent_framework;
    using namespace agent_framework::harness;
    CallbackProductionWorkflowInputAssembler inputs;
    HarnessStageRequest request;
    request.checkpoint.metadata = phase4_harness_test::metadata();
    std::string error;
    assert(!inputs.cognition(request, &error) && !error.empty());
    error.clear(); assert(!inputs.memory(request, &error) && !error.empty());
    error.clear(); assert(!inputs.assurance(request, false, &error) && !error.empty());
    error.clear(); assert(!inputs.remediation(request, &error) && !error.empty());
    error.clear(); assert(!inputs.judge(request, &error) && !error.empty());

    EmptyRepository repository;
    planning::InMemoryPlanStore plans;
    memory_v2::MemoryScope subject;
    subject.tenant_id = "tenant-a"; subject.task_id = "task-a"; subject.run_id = "run-harness";
    StoreBackedProductionWorkflowInputAssembler store_inputs(repository, plans, subject);
    error.clear(); assert(!store_inputs.cognition(request, &error) && !error.empty());
    error.clear(); assert(!store_inputs.memory(request, &error) && !error.empty());
    error.clear(); assert(!store_inputs.assurance(request, false, &error) && !error.empty());
    error.clear(); assert(!store_inputs.remediation(request, &error) && !error.empty());
    error.clear(); assert(!store_inputs.judge(request, &error) && !error.empty());

    planning::TaskIntake intake;
    intake.metadata = request.checkpoint.metadata;
    intake.user_goal = "verify dynamic context binding";
    repository.intake_value = intake;
    request.checkpoint.harness_id = "dynamic-context";
    request.checkpoint.revision = 7;
    request.checkpoint.pins.plan_digest = "sha256:plan";
    request.checkpoint.pins.approval_decision_id = "approval-1";
    request.checkpoint.pins.artifact_manifest_digest = "sha256:artifact";
    request.checkpoint.metadata.extensions["context_projection_required"] = true;
    request.checkpoint.metadata.extensions["context_projection_digest"] =
        "sha256:base-projection";
    error.clear();
    const auto dynamic = store_inputs.cognition(request, &error);
    assert(dynamic && error.empty());
    const auto first_digest = dynamic->intake.metadata.extensions.at(
        "context_projection_digest").get<std::string>();
    assert(!first_digest.empty() && first_digest != "sha256:base-projection");
    assert(dynamic->intake.metadata.extensions.at(
        "context_projection_base_digest") == "sha256:base-projection");
    request.checkpoint.revision = 8;
    request.checkpoint.pins.acceptance_report_digest = "sha256:report";
    const auto advanced = store_inputs.cognition(request, &error);
    assert(advanced);
    assert(advanced->intake.metadata.extensions.at(
        "context_projection_digest").get<std::string>() != first_digest);

    auto store = std::make_shared<llm_runtime::InMemoryLLMRuntimeStore>();
    llm_runtime::LLMInvocationManifest manifest;
    manifest.metadata = phase4_harness_test::metadata();
    manifest.invocation_id = "invocation-a";
    manifest.state = llm_runtime::InvocationState::Pending;
    manifest.role = "planner";
    manifest.profile_id = "planner";
    manifest.profile_revision = "v1";
    manifest.prompt_id = "planner";
    manifest.prompt_revision = "v1";
    manifest.prompt_digest = "sha256:prompt";
    manifest.route_decision_digest = "sha256:route";
    manifest.candidate_id = "candidate";
    manifest.provider = "provider";
    manifest.model = "model";
    manifest.adapter_revision = "v1";
    manifest.reasoning_effort = "high";
    manifest.independence_group = "group-a";
    manifest.evidence_authority = "candidate";
    manifest.calibration_revision = "cal-v1";
    manifest.memory_snapshot_id = "snapshot";
    manifest.memory_view_profile = "planning";
    manifest.memory_view_digest = "sha256:view";
    manifest.capability_digest = "sha256:capability";
    manifest.input_digest = "sha256:input";
    manifest.output_digest = "sha256:output";
    manifest.started_at = "2026-08-12T00:00:00Z";
    manifest.finished_at = "2026-08-12T00:00:01Z";
    assert(store->create_invocation(manifest).ok());
    manifest.state = llm_runtime::InvocationState::Running;
    assert(store->update_invocation(manifest, 1).ok());
    manifest.state = llm_runtime::InvocationState::Succeeded;
    assert(store->update_invocation(manifest, 2).ok());
    InvocationManifestResolver resolver(store);
    error.clear();
    const auto found = resolver.resolve("tenant-a", {"invocation-a"}, &error);
    assert(error.empty() && found.size() == 1);
    error.clear();
    assert(resolver.resolve("tenant-a", {"missing"}, &error).empty() && !error.empty());
}
