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

class TraceObserver final : public harness::HarnessCheckpointObserver {
public:
    TraceObserver(harness::HarnessStore& store, std::string tenant,
                  std::string harness_id, HarnessTurnAdapter::TraceSink sink)
        : store_(store), tenant_(std::move(tenant)),
          harness_id_(std::move(harness_id)), sink_(std::move(sink)) {}

    bool committed(const harness::HarnessCheckpoint&, std::string_view,
                   std::string* error) override {
        try {
            for(const auto& event : store_.events(tenant_, harness_id_, cursor_)) {
                sink_(event);
                cursor_ = event.sequence;
            }
            return true;
        } catch(const std::exception& exception) {
            if(error) *error = exception.what();
            return false;
        }
    }

private:
    harness::HarnessStore& store_;
    std::string tenant_;
    std::string harness_id_;
    HarnessTurnAdapter::TraceSink sink_;
    std::uint64_t cursor_{0};
};
}

HarnessTurnAdapter::HarnessTurnAdapter(
    harness::HarnessStore& store, Execution execution, ProjectionSink projection,
    TraceSink trace)
    : store_(store), execution_(std::move(execution)), projection_(std::move(projection)),
      trace_(std::move(trace)) {
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
    bind(harness::HarnessStage::Cognition, [&request](const auto&) {
        harness::HarnessStageResult out; out.outcome = harness::StageOutcome::Succeeded;
        out.public_output = {
            {"schema", "agent.lightweight_conversation_plan/v1"},
            {"authority", "non_authoritative"},
            {"profile", name(request.turn.profile)},
            {"objective", request.turn.input},
            {"steps", nlohmann::json::array({
                {{"id", "respond"}, {"action", "produce a relevant response"}},
                {{"id", "handoff"}, {"action", "return a candidate without claiming verification"}}
            })},
            {"acceptance_criteria", nlohmann::json::array({
                {{"id", "response-produced"},
                 {"requirement", "a candidate response is produced"}}
            })}
        };
        out.pins.plan_digest = digest(out.public_output);
        out.output_digest = out.pins.plan_digest; return out;
    });
    bind(harness::HarnessStage::PlanApproval, [](const auto& r) {
        harness::HarnessStageResult out; out.outcome = harness::StageOutcome::Succeeded;
        out.pins.approval_decision_id = "interactive-policy:" + r.checkpoint.harness_id;
        out.public_output = {{"authority", "non_authoritative"},
                             {"decision", "conversation_execution_allowed"}};
        out.output_digest = digest({{"policy", "interactive-nonproduction"},
                                    {"plan", r.checkpoint.pins.plan_digest}}); return out;
    });
    bind(harness::HarnessStage::Execution, [&](const auto& r) {
        model = execution_(request);
        harness::HarnessStageResult out;
        const bool has_delivery = !model.candidate_answer.empty() ||
                                  !model.tool_receipt_refs.empty();
        out.outcome = model.reason == ModelTurnStopReason::EndTurn && has_delivery
            ? harness::StageOutcome::Succeeded : harness::StageOutcome::Failed;
        out.pins.artifact_manifest_digest = digest({{"turn", request.turn.turn_id},
            {"answer", model.candidate_answer}, {"request", r.request_digest}});
        out.output_digest = out.pins.artifact_manifest_digest;
        if(out.outcome != harness::StageOutcome::Succeeded) {
            out.error_code = model.reason == ModelTurnStopReason::EndTurn
                ? "interactive_execution_empty_delivery"
                : "interactive_execution_failed";
            out.error_message = user_facing_turn_failure(out.error_code);
            out.public_output = {{"error_code", out.error_code},
                                 {"user_message", out.error_message}};
        }
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
        out.public_output = {{"scope", "response_pipeline_only"},
                             {"task_verification", false},
                             {"authority", "non_authoritative"}};
        out.pins.acceptance_report_digest = digest({{"artifact", r.checkpoint.pins.artifact_manifest_digest},
                                                     {"scope", "interactive-assurance"}});
        out.output_digest = out.pins.acceptance_report_digest; return out;
    });
    bind(harness::HarnessStage::Judge, [](const auto& r) {
        harness::HarnessStageResult out; out.outcome = harness::StageOutcome::Succeeded;
        out.pins.judge_report_digest = digest({{"acceptance", r.checkpoint.pins.acceptance_report_digest},
                                               {"judge", "interactive"}});
        out.public_output = {{"scope", "response_pipeline_only"},
                             {"task_verification", false},
                             {"authority", "non_authoritative"}};
        out.output_digest = out.pins.judge_report_digest; return out;
    });
    bind(harness::HarnessStage::Operations, [](const auto& r) {
        harness::HarnessStageResult out; out.outcome = harness::StageOutcome::Succeeded;
        out.pins.operations_snapshot_digest = digest({{"harness", r.checkpoint.harness_id},
                                                       {"revision", r.checkpoint.revision}});
        out.output_digest = out.pins.operations_snapshot_digest; return out;
    });

    harness::HarnessStart start;
    start.metadata.identity.tenant_id = request.turn.identity.tenant_id;
    start.metadata.identity.task_id = request.turn.task_id.empty()
        ? request.turn.turn_id : request.turn.task_id;
    start.metadata.identity.run_id = request.turn.run_id.empty()
        ? request.turn.turn_id : request.turn.run_id;
    start.metadata.extensions["conversation_id"] = request.turn.identity.conversation_id;
    // Completing this harness only proves that the interactive response pipeline
    // ran.  It never grants task-closure authority.
    start.metadata.extensions["completion_semantics"] =
        "pipeline_completed_unverified";
    start.harness_id = "turn:" + request.turn.turn_id;
    start.intake_digest = digest({{"input", request.turn.input},
                                  {"profile", name(request.turn.profile)}});
    start.acceptance_contract_digest = digest({{"profile", name(request.turn.profile)},
                                                {"authority", "harness"}});
    start.profile_revision_digest = digest({{"profile", name(request.turn.profile)}});
    start.prompt_revision_digest = digest({{"prompt", "interactive-harness-v1"}});
    std::shared_ptr<harness::HarnessCheckpointObserver> observer;
    if(trace_) observer = std::make_shared<TraceObserver>(
        store_, start.metadata.identity.tenant_id, start.harness_id, trace_);
    harness::Phase4HarnessRuntime runtime(
        store_, std::move(ports), std::move(observer));
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
        // Keep any streamed candidate. Never promote an internal error code
        // into user-visible answer content; the UI maps the failure separately.
        return model;
    }
    // Structural harness completion is not semantic task verification.
    model.task_completion_verified = false;
    return model;
}

}  // namespace agent_framework::conversation
