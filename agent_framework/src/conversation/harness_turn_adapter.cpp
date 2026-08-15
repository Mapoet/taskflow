#include "agent/conversation/harness_turn_adapter.hpp"

#include <chrono>
#include <stdexcept>

#include "agent/contracts/contract.hpp"

namespace agent_framework::conversation {
namespace {
std::string digest(const nlohmann::json& value) {
    return contracts::canonical_digest(value).value_or("sha256:unavailable");
}
std::string now() {
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
class StagePort final : public harness::HarnessStagePort {
public:
    using Run = std::function<harness::HarnessStageResult(const harness::HarnessStageRequest&)>;
    StagePort(std::string id, Run run) : id_(std::move(id)), run_(std::move(run)) {}
    std::string id() const override { return id_; }
    bool may_have_side_effects() const noexcept override { return false; }
    harness::HarnessStageResult execute(const harness::HarnessStageRequest& request) override {
        return run_(request);
    }
private:
    std::string id_;
    Run run_;
};
}

HarnessTurnAdapter::HarnessTurnAdapter(
    harness::HarnessStore& store, Execution execution, ProjectionSink projection)
    : store_(store), execution_(std::move(execution)), projection_(std::move(projection)) {
    if(!execution_) throw std::invalid_argument("harness turn execution stage is required");
}

ModelTurnOutcome HarnessTurnAdapter::execute(const HarnessSupportedTurnRequest& request) {
    ModelTurnOutcome model;
    harness::HarnessPortRegistry ports;
    const auto bind = [&](harness::HarnessStage stage, auto run) {
        if(!ports.bind(stage, std::make_shared<StagePort>(
               "interactive.harness." + harness::harness_stage_name(stage), std::move(run))))
            throw std::runtime_error("cannot bind interactive harness stage");
    };
    bind(harness::HarnessStage::Intake, [](const auto& r) {
        harness::HarnessStageResult out; out.outcome = harness::StageOutcome::Succeeded;
        out.output_digest = r.checkpoint.pins.intake_digest; return out;
    });
    bind(harness::HarnessStage::Cognition, [](const auto& r) {
        harness::HarnessStageResult out; out.outcome = harness::StageOutcome::Succeeded;
        out.pins.plan_digest = digest({{"request", r.request_digest}, {"stage", "cognition"}});
        out.output_digest = out.pins.plan_digest; return out;
    });
    bind(harness::HarnessStage::PlanApproval, [](const auto& r) {
        harness::HarnessStageResult out; out.outcome = harness::StageOutcome::Succeeded;
        out.pins.approval_decision_id = "interactive-policy:" + r.checkpoint.harness_id;
        out.output_digest = digest({{"policy", "interactive-nonproduction"},
                                    {"plan", r.checkpoint.pins.plan_digest}}); return out;
    });
    bind(harness::HarnessStage::Execution, [&](const auto& r) {
        model = execution_(request);
        harness::HarnessStageResult out;
        out.outcome = model.reason == ModelTurnStopReason::EndTurn
            ? harness::StageOutcome::Succeeded : harness::StageOutcome::Failed;
        out.pins.artifact_manifest_digest = digest({{"turn", request.turn.turn_id},
            {"answer", model.candidate_answer}, {"request", r.request_digest}});
        out.output_digest = out.pins.artifact_manifest_digest;
        if(out.outcome != harness::StageOutcome::Succeeded)
            out.error_code = "interactive_execution_failed";
        return out;
    });
    bind(harness::HarnessStage::MemoryUpdate, [](const auto& r) {
        harness::HarnessStageResult out; out.outcome = harness::StageOutcome::Succeeded;
        out.pins.memory_snapshot_id = "interactive-memory:" + r.checkpoint.harness_id;
        out.pins.memory_view_digest = digest({{"harness", r.checkpoint.harness_id},
                                               {"stage", "memory"}});
        out.output_digest = out.pins.memory_view_digest; return out;
    });
    bind(harness::HarnessStage::Assurance, [](const auto& r) {
        harness::HarnessStageResult out; out.outcome = harness::StageOutcome::Succeeded;
        out.acceptance_decision = "accepted";
        out.pins.acceptance_report_digest = digest({{"artifact", r.checkpoint.pins.artifact_manifest_digest},
                                                     {"scope", "interactive-assurance"}});
        out.output_digest = out.pins.acceptance_report_digest; return out;
    });
    bind(harness::HarnessStage::Judge, [](const auto& r) {
        harness::HarnessStageResult out; out.outcome = harness::StageOutcome::Succeeded;
        out.pins.judge_report_digest = digest({{"acceptance", r.checkpoint.pins.acceptance_report_digest},
                                               {"judge", "interactive"}});
        out.output_digest = out.pins.judge_report_digest; return out;
    });
    bind(harness::HarnessStage::Operations, [](const auto& r) {
        harness::HarnessStageResult out; out.outcome = harness::StageOutcome::Succeeded;
        out.pins.operations_snapshot_digest = digest({{"harness", r.checkpoint.harness_id},
                                                       {"revision", r.checkpoint.revision}});
        out.output_digest = out.pins.operations_snapshot_digest; return out;
    });

    harness::Phase4HarnessRuntime runtime(store_, std::move(ports));
    harness::HarnessStart start;
    start.metadata.identity.tenant_id = request.turn.identity.tenant_id;
    start.metadata.identity.task_id = request.turn.turn_id;
    start.metadata.identity.run_id = request.turn.turn_id;
    start.metadata.extensions["conversation_id"] = request.turn.identity.conversation_id;
    start.harness_id = "turn:" + request.turn.turn_id;
    start.intake_digest = digest({{"input", request.turn.input},
                                  {"profile", name(request.turn.profile)}});
    start.acceptance_contract_digest = digest({{"profile", name(request.turn.profile)},
                                                {"authority", "harness"}});
    start.profile_revision_digest = digest({{"profile", name(request.turn.profile)}});
    start.prompt_revision_digest = digest({{"prompt", "interactive-harness-v1"}});
    harness::HarnessRuntimeOptions options;
    options.now = now;
    const auto result = runtime.run(start, options);
    if(projection_) {
        auto snapshot = harness::Phase4HarnessRuntime::project_operations(result.checkpoint);
        snapshot.task_completion_verified = false;
        snapshot.completion_authority = "none";
        snapshot.task_closure_state = result.state == harness::HarnessState::Completed
            ? "execution_completed_unverified" : "execution_incomplete";
        if(result.state == harness::HarnessState::Completed) {
            snapshot.overall_status = OperationsStatus::Running;
            snapshot.summary = "Interactive execution completed; task closure not evaluated";
        }
        projection_(snapshot);
    }
    if(result.state != harness::HarnessState::Completed) {
        model.reason = ModelTurnStopReason::GuardStopped;
        model.task_completion_verified = false;
        if(model.candidate_answer.empty())
            model.candidate_answer = result.error_code.empty()
                ? result.checkpoint.terminal_reason : result.error_code;
        return model;
    }
    // Structural harness completion is not semantic task verification.
    model.task_completion_verified = false;
    return model;
}

}  // namespace agent_framework::conversation
