#include <cassert>
#include <filesystem>

#include "agent/harness/production_composition.hpp"
#include "agent/harness/workflow_adapter.hpp"
#include "agent/internal/platform_io.hpp"
#include "phase4_harness_test_support.hpp"

namespace {
using namespace agent_framework;
class TestWorkflowAdapter final : public harness::TypedWorkflowAdapter {
public:
    explicit TestWorkflowAdapter(harness::HarnessStage stage, bool emit_invocation = true)
        : stage_(stage), emit_invocation_(emit_invocation) {}
    std::string id() const override { return "typed-" + harness::harness_stage_name(stage_); }
    harness::WorkflowAdapterKind kind() const noexcept override {
        using K = harness::WorkflowAdapterKind;
        switch(stage_) {
            case harness::HarnessStage::Intake: return K::Intake;
            case harness::HarnessStage::Cognition: return K::Cognition;
            case harness::HarnessStage::PlanApproval: return K::Approval;
            case harness::HarnessStage::Execution:
            case harness::HarnessStage::Reexecution: return K::Execution;
            case harness::HarnessStage::MemoryUpdate: return K::Memory;
            case harness::HarnessStage::Assurance: return K::Assurance;
            case harness::HarnessStage::Remediation: return K::Remediation;
            case harness::HarnessStage::Reverification: return K::Reverification;
            case harness::HarnessStage::Judge: return K::Judge;
            case harness::HarnessStage::Operations:
            case harness::HarnessStage::Complete: return K::Operations;
        }
        return K::Operations;
    }
    harness::HarnessStage stage() const noexcept override { return stage_; }
    bool side_effecting() const noexcept override {
        return stage_ == harness::HarnessStage::Execution ||
               stage_ == harness::HarnessStage::Reexecution;
    }
    std::string implementation_revision() const override { return "test-workflow-v1"; }
    std::string configuration_digest() const override { return "sha256:test-config"; }
    harness::WorkflowStageExecution run(const harness::HarnessStageRequest& request) override {
        harness::WorkflowStageExecution value;
        value.result = phase4_harness_test::successful(request, false);
        if(emit_invocation_ && (kind() == harness::WorkflowAdapterKind::Cognition ||
           kind() == harness::WorkflowAdapterKind::Memory ||
           kind() == harness::WorkflowAdapterKind::Assurance ||
           kind() == harness::WorkflowAdapterKind::Remediation ||
           kind() == harness::WorkflowAdapterKind::Reverification ||
           kind() == harness::WorkflowAdapterKind::Judge)) {
            llm_runtime::LLMInvocationManifest manifest;
            manifest.metadata = request.checkpoint.metadata;
            manifest.invocation_id = request.effect_id + ":llm";
            manifest.state = llm_runtime::InvocationState::Succeeded;
            manifest.role = harness::harness_stage_name(request.stage);
            manifest.profile_id = "profile"; manifest.profile_revision = "v1";
            manifest.prompt_id = "prompt"; manifest.prompt_revision = "v1";
            manifest.provider = "provider"; manifest.model = "model";
            manifest.route_decision_digest = "sha256:route";
            manifest.calibration_revision = "cal-v1";
            manifest.latency_ms = 10;
            value.invocations.push_back(std::move(manifest));
        }
        return value;
    }
    std::optional<harness::WorkflowStageExecution> reconcile(
        const harness::HarnessStageRequest& request) override {
        return side_effecting() ? std::optional(run(request)) : std::nullopt;
    }
private:
    harness::HarnessStage stage_;
    bool emit_invocation_{true};
};
}

int main() {
    using namespace agent_framework;
    using namespace agent_framework::harness;
    using namespace phase4_harness_test;
    const auto root = std::filesystem::temp_directory_path() /
        ("phase4-production-composition-" + std::to_string(internal::current_process_id()));
    std::error_code ec; std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    SQLiteHarnessStore harnesses((root / "harness.sqlite3").string());
    run::SQLiteRunStore runs((root / "run.sqlite3").string());
    Phase4ProductionComposition composition(harnesses, runs);
    const auto empty = composition.validate();
    assert(!empty.ready && empty.issues.size() == 11);

    auto sink = std::make_shared<telemetry::InMemoryTelemetrySink>();
    auto telemetry_runtime = std::make_shared<telemetry::TelemetryRuntime>(sink,
        telemetry::TelemetryPolicy{{"gen_ai.operation.name", "gen_ai.provider.name",
            "gen_ai.request.model", "gen_ai.response.model", "agent.role",
            "agent.profile.id", "agent.profile.revision", "agent.prompt.id",
            "agent.prompt.revision", "agent.route.digest", "agent.calibration.revision",
            "agent.fallback.count", "agent.harness.stage",
            "agent.invocation.manifest_digest"}, 256});
    auto observer = std::make_shared<TelemetryLLMInvocationObserver>(*telemetry_runtime);

    // A manifest-bearing callback remains non-production and cannot spoof typed wiring.
    Phase4ProductionComposition spoofed(harnesses, runs);
    assert(spoofed.bind(HarnessStage::Intake, std::make_shared<ManifestHarnessStagePort>(
        "spoof", false, "scripted", "sha256:spoof",
        [](const HarnessStageRequest& request) { return successful(request, false); })));
    assert(!spoofed.validate().ready);

    WorkflowHarnessStagePort missing_evidence(
        std::make_shared<TestWorkflowAdapter>(HarnessStage::Cognition, false), observer);
    HarnessStageRequest missing_request;
    missing_request.checkpoint.metadata = metadata();
    missing_request.stage = HarnessStage::Cognition;
    missing_request.effect_id = "missing-evidence";
    const auto missing = missing_evidence.execute(missing_request);
    assert(missing.outcome == StageOutcome::ManualReview);
    assert(missing.error_code == "llm_invocation_evidence_missing");

    for(std::size_t index = 0; index < static_cast<std::size_t>(HarnessStage::Complete); ++index) {
        const auto stage = static_cast<HarnessStage>(index);
        assert(composition.bind(stage, std::make_shared<WorkflowHarnessStagePort>(
            std::make_shared<TestWorkflowAdapter>(stage), observer)));
    }
    const auto ready = composition.validate();
    assert(ready.ready && !ready.manifest_digest.empty());
    std::string error;
    auto runtime = composition.build(&error);
    assert(runtime && error.empty());
    auto harness_start = start("production-harness");
    const auto executed = runtime->run(harness_start);
    assert(executed.state == HarnessState::Completed);
    assert(sink->spans().size() == 4);
    assert(sink->metrics().size() == 4);
    for(const auto& span : sink->spans()) {
        assert(span.name == "gen_ai.role.invocation");
        assert(!span.attributes.at("agent.invocation.manifest_digest").empty());
        assert(!span.context.trace_id.empty() && !span.context.span_id.empty());
    }
    std::filesystem::remove_all(root, ec);
    return 0;
}
