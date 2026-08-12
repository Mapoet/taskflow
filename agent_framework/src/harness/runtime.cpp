#include "agent/harness/runtime.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace agent_framework::harness {
namespace {
using json = nlohmann::json;

bool terminal(HarnessState state) {
    return state == HarnessState::ManualReview || state == HarnessState::Completed ||
           state == HarnessState::Rejected || state == HarnessState::Failed ||
           state == HarnessState::Cancelled;
}

std::string now_value(const HarnessRuntimeOptions& options) {
    return options.now ? options.now() : "1970-01-01T00:00:00Z";
}

std::string digest_or_throw(const json& value) {
    const auto digest = contracts::canonical_digest(value);
    if(!digest) throw std::runtime_error("canonical digest failed");
    return *digest;
}

std::string request_digest(const HarnessCheckpoint& checkpoint, HarnessStage stage,
                           std::uint64_t attempt) {
    return digest_or_throw({{"harness_id", checkpoint.harness_id},
                            {"revision", checkpoint.revision},
                            {"stage", harness_stage_name(stage)},
                            {"attempt", attempt},
                            {"remediation_cycle", checkpoint.remediation_cycle},
                            {"pins", {{"intake", checkpoint.pins.intake_digest},
                                      {"plan", checkpoint.pins.plan_digest},
                                      {"acceptance", checkpoint.pins.acceptance_contract_digest},
                                      {"memory_snapshot", checkpoint.pins.memory_snapshot_id},
                                      {"memory_view", checkpoint.pins.memory_view_digest},
                                      {"approval", checkpoint.pins.approval_decision_id},
                                      {"artifact", checkpoint.pins.artifact_manifest_digest},
                                      {"report", checkpoint.pins.acceptance_report_digest}}}});
}

HarnessEvent event_for(const HarnessCheckpoint& checkpoint, std::string type,
                       json payload, const HarnessRuntimeOptions& options) {
    HarnessEvent event;
    event.harness_id = checkpoint.harness_id;
    event.sequence = checkpoint.revision;
    event.checkpoint_revision = checkpoint.revision;
    event.event_type = std::move(type);
    event.payload = std::move(payload);
    event.created_at = now_value(options);
    return event;
}

void assign_if_present(std::string& target, const std::string& value,
                       bool replace_allowed, std::string_view name) {
    if(value.empty()) return;
    if(!target.empty() && target != value && !replace_allowed)
        throw std::invalid_argument(std::string("pinned revision changed unexpectedly: ") +
                                    std::string(name));
    target = value;
}

void merge_pins(PinnedRevisions& target, const PinnedRevisions& incoming,
                HarnessStage stage) {
    assign_if_present(target.intake_digest, incoming.intake_digest, false, "intake");
    assign_if_present(target.plan_digest, incoming.plan_digest,
                      stage == HarnessStage::Remediation, "plan");
    assign_if_present(target.acceptance_contract_digest,
                      incoming.acceptance_contract_digest, false, "acceptance_contract");
    assign_if_present(target.memory_snapshot_id, incoming.memory_snapshot_id,
                      stage == HarnessStage::MemoryUpdate, "memory_snapshot");
    assign_if_present(target.memory_view_digest, incoming.memory_view_digest,
                      stage == HarnessStage::MemoryUpdate, "memory_view");
    assign_if_present(target.profile_revision_digest, incoming.profile_revision_digest,
                      false, "profile_revision");
    assign_if_present(target.prompt_revision_digest, incoming.prompt_revision_digest,
                      false, "prompt_revision");
    assign_if_present(target.approval_decision_id, incoming.approval_decision_id,
                      stage == HarnessStage::PlanApproval ||
                          stage == HarnessStage::Remediation,
                      "approval_decision");
    assign_if_present(target.artifact_manifest_digest,
                      incoming.artifact_manifest_digest,
                      stage == HarnessStage::Execution ||
                          stage == HarnessStage::Reexecution,
                      "artifact_manifest");
    assign_if_present(target.acceptance_report_digest,
                      incoming.acceptance_report_digest,
                      stage == HarnessStage::Assurance ||
                          stage == HarnessStage::Reverification,
                      "acceptance_report");
    assign_if_present(target.judge_report_digest, incoming.judge_report_digest,
                      stage == HarnessStage::Judge, "judge_report");
    assign_if_present(target.operations_snapshot_digest,
                      incoming.operations_snapshot_digest,
                      stage == HarnessStage::Operations, "operations_snapshot");
}

std::uint64_t next_attempt(const HarnessCheckpoint& checkpoint, HarnessStage stage) {
    std::uint64_t attempt = 1;
    for(const auto& record : checkpoint.stage_records)
        if(record.stage == stage) attempt = std::max(attempt, record.attempt + 1);
    return attempt;
}

HarnessOutboxEntry* pending_for(HarnessCheckpoint& checkpoint, HarnessStage stage) {
    for(auto found = checkpoint.outbox.rbegin(); found != checkpoint.outbox.rend(); ++found)
        if(found->stage == stage && found->state == OutboxState::Pending) return &*found;
    return nullptr;
}

HarnessStage next_linear(HarnessStage stage) {
    switch(stage) {
        case HarnessStage::Intake: return HarnessStage::Cognition;
        case HarnessStage::Cognition: return HarnessStage::PlanApproval;
        case HarnessStage::PlanApproval: return HarnessStage::Execution;
        case HarnessStage::Execution: return HarnessStage::MemoryUpdate;
        case HarnessStage::MemoryUpdate: return HarnessStage::Assurance;
        case HarnessStage::Remediation: return HarnessStage::PlanApproval;
        case HarnessStage::Reexecution: return HarnessStage::Reverification;
        case HarnessStage::Judge: return HarnessStage::Operations;
        case HarnessStage::Operations:
        case HarnessStage::Assurance:
        case HarnessStage::Reverification:
        case HarnessStage::Complete: return HarnessStage::Complete;
    }
    return HarnessStage::Complete;
}

OperationsStatus operation_status(StageOutcome outcome) {
    switch(outcome) {
        case StageOutcome::Succeeded: return OperationsStatus::Passed;
        case StageOutcome::AwaitingApproval: return OperationsStatus::Pending;
        case StageOutcome::NeedsRemediation: return OperationsStatus::Warning;
        case StageOutcome::Rejected: return OperationsStatus::Blocked;
        case StageOutcome::Retryable: return OperationsStatus::Warning;
        case StageOutcome::ManualReview: return OperationsStatus::Blocked;
        case StageOutcome::Failed: return OperationsStatus::Failed;
        case StageOutcome::Cancelled: return OperationsStatus::Blocked;
    }
    return OperationsStatus::Unknown;
}
}  // namespace

CallbackHarnessStagePort::CallbackHarnessStagePort(
    std::string port_id, bool side_effecting, Execute execute, Reconcile reconcile)
    : id_(std::move(port_id)), side_effecting_(side_effecting),
      execute_(std::move(execute)), reconcile_(std::move(reconcile)) {
    if(id_.empty() || !execute_)
        throw std::invalid_argument("callback harness port requires id and execute callback");
}

HarnessStageResult CallbackHarnessStagePort::execute(const HarnessStageRequest& request) {
    return execute_(request);
}

std::optional<HarnessStageResult> CallbackHarnessStagePort::reconcile(
    const HarnessStageRequest& request) {
    return reconcile_ ? reconcile_(request) : std::nullopt;
}

bool HarnessPortRegistry::bind(HarnessStage stage,
                               std::shared_ptr<HarnessStagePort> port) {
    if(stage == HarnessStage::Complete || !port || port->id().empty()) return false;
    return ports_.emplace(stage, std::move(port)).second;
}

std::shared_ptr<HarnessStagePort> HarnessPortRegistry::find(HarnessStage stage) const {
    const auto found = ports_.find(stage);
    return found == ports_.end() ? nullptr : found->second;
}

std::vector<HarnessStage> HarnessPortRegistry::bound_stages() const {
    std::vector<HarnessStage> stages;
    for(const auto& [stage, port] : ports_) if(port) stages.push_back(stage);
    return stages;
}

Phase4HarnessRuntime::Phase4HarnessRuntime(HarnessStore& store, HarnessPortRegistry ports,
                                           std::shared_ptr<HarnessCheckpointObserver> observer)
    : store_(store), ports_(std::move(ports)), observer_(std::move(observer)) {}

HarnessRunResult Phase4HarnessRuntime::run(const HarnessStart& start,
                                           const HarnessRuntimeOptions& options) {
    if(start.metadata.identity.tenant_id.empty() ||
       start.metadata.identity.task_id.empty() || start.harness_id.empty() ||
       start.intake_digest.empty() || start.acceptance_contract_digest.empty() ||
       start.profile_revision_digest.empty() || start.prompt_revision_digest.empty() ||
       start.max_remediation_cycles == 0) {
        return {HarnessState::Failed, {}, "harness_start_invalid",
                "identity, harness, immutable digests, and positive remediation bound are required"};
    }
    HarnessCheckpoint checkpoint;
    checkpoint.metadata = start.metadata;
    checkpoint.harness_id = start.harness_id;
    checkpoint.revision = 1;
    checkpoint.state = HarnessState::Running;
    checkpoint.next_stage = HarnessStage::Intake;
    checkpoint.max_remediation_cycles = start.max_remediation_cycles;
    checkpoint.judge_required = start.judge_required;
    checkpoint.pins.intake_digest = start.intake_digest;
    checkpoint.pins.acceptance_contract_digest = start.acceptance_contract_digest;
    checkpoint.pins.profile_revision_digest = start.profile_revision_digest;
    checkpoint.pins.prompt_revision_digest = start.prompt_revision_digest;
    checkpoint.updated_at = now_value(options);
    auto commit = store_.create(checkpoint,
        event_for(checkpoint, "harness_created",
                  {{"stage", harness_stage_name(checkpoint.next_stage)}}, options));
    if(!commit) {
        if(commit.status == HarnessStoreStatus::AlreadyExists)
            return resume(start.metadata.identity.tenant_id, start.harness_id, options);
        return {HarnessState::Failed, checkpoint, "harness_create_failed", commit.error};
    }
    if(observer_) { std::string error; if(!observer_->committed(checkpoint,"harness_created",&error))
        return {HarnessState::ManualReview,checkpoint,"checkpoint_observer_failed",error}; }
    return drive(std::move(checkpoint), options);
}

HarnessRunResult Phase4HarnessRuntime::resume(std::string_view tenant_id,
                                              std::string_view harness_id,
                                              const HarnessRuntimeOptions& options) {
    auto stored = store_.load(tenant_id, harness_id);
    if(!stored)
        return {HarnessState::Failed, {}, "harness_not_found", "harness checkpoint not found"};
    if(stored->checkpoint.state == HarnessState::AwaitingApproval) {
        auto checkpoint = stored->checkpoint;
        checkpoint.revision += 1;
        checkpoint.state = HarnessState::Running;
        checkpoint.updated_at = now_value(options);
        auto commit = store_.compare_exchange(
            checkpoint, stored->revision,
            event_for(checkpoint, "harness_resumed",
                      {{"stage", harness_stage_name(checkpoint.next_stage)}}, options));
        if(!commit)
            return {HarnessState::ManualReview, stored->checkpoint,
                    "harness_resume_conflict", commit.error};
        return drive(std::move(checkpoint), options);
    }
    return drive(std::move(stored->checkpoint), options);
}

HarnessRunResult Phase4HarnessRuntime::drive(HarnessCheckpoint checkpoint,
                                             const HarnessRuntimeOptions& options) {
    auto persist = [&](std::string event_type, json payload) -> bool {
        const auto expected = checkpoint.revision;
        checkpoint.revision += 1;
        checkpoint.updated_at = now_value(options);
        auto commit = store_.compare_exchange(
            checkpoint, expected,
            event_for(checkpoint, event_type, std::move(payload), options));
        if(commit) {
            if(observer_) { std::string error; if(!observer_->committed(checkpoint,event_type,&error)) {
                checkpoint.state=HarnessState::ManualReview;
                checkpoint.terminal_reason="checkpoint_observer_failed:"+error;
                return false;
            }}
            return true;
        }
        checkpoint.revision = expected;
        checkpoint.state = HarnessState::ManualReview;
        checkpoint.terminal_reason = commit.status == HarnessStoreStatus::RevisionConflict
            ? "stale_harness_writer" : "harness_store_commit_failed";
        return false;
    };

    std::uint64_t transitions = 0;
    while(!terminal(checkpoint.state) && transitions++ < options.max_transitions_per_run) {
        if(options.cancelled && options.cancelled()) {
            checkpoint.state = HarnessState::Cancelled;
            checkpoint.terminal_reason = "cancelled";
            persist("harness_cancelled", json::object());
            break;
        }
        const auto stage = checkpoint.next_stage;
        if(stage == HarnessStage::Complete) {
            const auto issues = completion_gate_issues(checkpoint);
            if(!issues.empty()) {
                checkpoint.state = HarnessState::ManualReview;
                checkpoint.terminal_reason = "completion_gate_failed";
                persist("completion_gate_failed", {{"issues", issues}});
            } else {
                checkpoint.state = HarnessState::Completed;
                checkpoint.terminal_reason = "accepted";
                persist("harness_completed", json::object());
            }
            break;
        }
        const auto port = ports_.find(stage);
        if(!port) {
            checkpoint.state = HarnessState::ManualReview;
            checkpoint.terminal_reason = "required_stage_port_missing:" + harness_stage_name(stage);
            persist("stage_port_missing", {{"stage", harness_stage_name(stage)}});
            break;
        }

        HarnessStageRequest request;
        request.checkpoint = checkpoint;
        request.stage = stage;
        request.cancelled = options.cancelled;
        HarnessStageResult result;
        HarnessOutboxEntry* pending = pending_for(checkpoint, stage);
        if(pending) {
            request.attempt = pending->attempt;
            request.effect_id = pending->effect_id;
            request.idempotency_key = pending->idempotency_key;
            request.request_digest = pending->request_digest;
            if(port->may_have_side_effects()) {
                auto reconciled = port->reconcile(request);
                if(!reconciled) {
                    pending->state = OutboxState::Unknown;
                    pending->error = "effect outcome could not be reconciled";
                    checkpoint.state = HarnessState::ManualReview;
                    checkpoint.terminal_reason = "unknown_external_effect";
                    persist("effect_unknown", {{"effect_id", pending->effect_id},
                                                {"stage", harness_stage_name(stage)}});
                    break;
                }
                result = std::move(*reconciled);
            } else {
                result = port->execute(request);
            }
        } else {
            request.attempt = next_attempt(checkpoint, stage);
            request.effect_id = checkpoint.harness_id + ":" + harness_stage_name(stage) + ":" +
                                std::to_string(checkpoint.remediation_cycle) + ":" +
                                std::to_string(request.attempt);
            request.idempotency_key = request.effect_id;
            request.request_digest = request_digest(checkpoint, stage, request.attempt);
            checkpoint.outbox.push_back({request.effect_id, stage, request.attempt,
                request.idempotency_key, request.request_digest, OutboxState::Pending, {}, {}});
            if(!persist("outbox_prepared", {{"effect_id", request.effect_id},
                                             {"stage", harness_stage_name(stage)},
                                             {"request_digest", request.request_digest}})) break;
            request.checkpoint = checkpoint;
            result = port->execute(request);
            if(options.after_stage_effect) options.after_stage_effect(stage);
            pending = pending_for(checkpoint, stage);
        }

        pending = pending_for(checkpoint, stage);
        if(!pending) {
            checkpoint.state = HarnessState::ManualReview;
            checkpoint.terminal_reason = "outbox_entry_lost";
            persist("outbox_entry_lost", {{"stage", harness_stage_name(stage)}});
            break;
        }
        if(port->may_have_side_effects() && result.outcome == StageOutcome::Succeeded &&
           result.effect_receipt_digest.empty()) {
            result.outcome = StageOutcome::ManualReview;
            result.error_code = "effect_receipt_missing";
            result.error_message = "side-effecting stage did not return a receipt digest";
        }
        pending->receipt_digest = result.effect_receipt_digest;
        pending->state = result.outcome == StageOutcome::Succeeded ||
                                 result.outcome == StageOutcome::AwaitingApproval ||
                                 result.outcome == StageOutcome::NeedsRemediation
            ? OutboxState::Committed
            : OutboxState::Rejected;
        pending->error = result.error_message;

        try { merge_pins(checkpoint.pins, result.pins, stage); }
        catch(const std::exception& error) {
            result.outcome = StageOutcome::ManualReview;
            result.error_code = "pin_conflict";
            result.error_message = error.what();
            pending->state = OutboxState::Unknown;
        }
        checkpoint.stage_records.push_back({stage, request.attempt, result.outcome,
            request.effect_id, result.invocation_manifest_digest, result.output_digest,
            result.finding_ids, result.error_code, result.error_message});

        switch(result.outcome) {
            case StageOutcome::Succeeded:
                if(stage == HarnessStage::Assurance || stage == HarnessStage::Reverification) {
                    if(result.acceptance_decision != "accepted" ||
                       checkpoint.pins.acceptance_report_digest.empty()) {
                        checkpoint.state = HarnessState::ManualReview;
                        checkpoint.terminal_reason = "assurance_success_without_accepted_report";
                    } else {
                        checkpoint.unresolved_findings.clear();
                        checkpoint.next_stage = checkpoint.judge_required
                            ? HarnessStage::Judge : HarnessStage::Operations;
                    }
                } else if(stage == HarnessStage::PlanApproval &&
                          checkpoint.remediation_cycle > 0 &&
                          !checkpoint.unresolved_findings.empty()) {
                    checkpoint.next_stage = HarnessStage::Reexecution;
                } else {
                    checkpoint.next_stage = next_linear(stage);
                }
                break;
            case StageOutcome::AwaitingApproval:
                checkpoint.state = HarnessState::AwaitingApproval;
                checkpoint.terminal_reason = "approval_required";
                break;
            case StageOutcome::NeedsRemediation:
                checkpoint.unresolved_findings = result.finding_ids;
                if(stage != HarnessStage::Assurance && stage != HarnessStage::Reverification) {
                    checkpoint.state = HarnessState::ManualReview;
                    checkpoint.terminal_reason = "remediation_requested_by_invalid_stage";
                } else if(checkpoint.remediation_cycle >= checkpoint.max_remediation_cycles) {
                    checkpoint.state = HarnessState::ManualReview;
                    checkpoint.terminal_reason = "remediation_cycle_limit";
                } else {
                    ++checkpoint.remediation_cycle;
                    // An artifact finding invalidates the old acceptance closure and
                    // authorization. A new remediation plan must be approved and verified.
                    checkpoint.pins.acceptance_report_digest.clear();
                    checkpoint.pins.approval_decision_id.clear();
                    checkpoint.next_stage = HarnessStage::Remediation;
                }
                break;
            case StageOutcome::Rejected:
                checkpoint.state = HarnessState::Rejected;
                checkpoint.terminal_reason = result.error_code.empty()
                    ? "stage_rejected" : result.error_code;
                break;
            case StageOutcome::Retryable:
                if(request.attempt >= options.max_stage_attempts) {
                    checkpoint.state = HarnessState::ManualReview;
                    checkpoint.terminal_reason = "stage_retry_limit";
                }
                break;
            case StageOutcome::ManualReview:
                checkpoint.state = HarnessState::ManualReview;
                checkpoint.terminal_reason = result.error_code.empty()
                    ? "stage_manual_review" : result.error_code;
                break;
            case StageOutcome::Failed:
                checkpoint.state = HarnessState::Failed;
                checkpoint.terminal_reason = result.error_code.empty()
                    ? "stage_failed" : result.error_code;
                break;
            case StageOutcome::Cancelled:
                checkpoint.state = HarnessState::Cancelled;
                checkpoint.terminal_reason = "cancelled";
                break;
        }
        if(!persist("stage_result", {{"stage", harness_stage_name(stage)},
                                     {"outcome", stage_outcome_name(result.outcome)},
                                     {"effect_id", request.effect_id},
                                     {"next_stage", harness_stage_name(checkpoint.next_stage)}}))
            break;
        if(checkpoint.state == HarnessState::AwaitingApproval) break;
    }
    if(!terminal(checkpoint.state) && transitions >= options.max_transitions_per_run) {
        checkpoint.state = HarnessState::ManualReview;
        checkpoint.terminal_reason = "transition_limit";
        persist("transition_limit", json::object());
    }
    return {checkpoint.state, checkpoint,
            checkpoint.state == HarnessState::Failed ||
                    checkpoint.state == HarnessState::ManualReview
                ? checkpoint.terminal_reason : std::string(),
            {}};
}

std::vector<std::string> Phase4HarnessRuntime::completion_gate_issues(
    const HarnessCheckpoint& checkpoint) {
    std::vector<std::string> issues;
    const auto required = [&](bool condition, std::string issue) {
        if(!condition) issues.push_back(std::move(issue));
    };
    required(!checkpoint.pins.intake_digest.empty(), "intake_digest_missing");
    required(!checkpoint.pins.plan_digest.empty(), "plan_digest_missing");
    required(!checkpoint.pins.acceptance_contract_digest.empty(),
             "acceptance_contract_digest_missing");
    required(!checkpoint.pins.memory_snapshot_id.empty() &&
             !checkpoint.pins.memory_view_digest.empty(), "memory_binding_missing");
    required(!checkpoint.pins.approval_decision_id.empty(), "approval_decision_missing");
    required(!checkpoint.pins.artifact_manifest_digest.empty(), "artifact_manifest_missing");
    required(!checkpoint.pins.acceptance_report_digest.empty(), "acceptance_report_missing");
    required(!checkpoint.judge_required || !checkpoint.pins.judge_report_digest.empty(),
             "judge_report_missing");
    required(!checkpoint.pins.operations_snapshot_digest.empty(),
             "operations_snapshot_missing");
    required(checkpoint.unresolved_findings.empty(), "unresolved_findings_present");
    for(const auto& entry : checkpoint.outbox)
        if(entry.state == OutboxState::Pending || entry.state == OutboxState::Unknown)
            issues.push_back("unresolved_effect:" + entry.effect_id);
    const auto succeeded = [&](HarnessStage stage) {
        return std::any_of(checkpoint.stage_records.begin(), checkpoint.stage_records.end(),
            [&](const auto& record) {
                return record.stage == stage && record.outcome == StageOutcome::Succeeded;
            });
    };
    for(const auto stage : {HarnessStage::Intake, HarnessStage::Cognition,
                            HarnessStage::PlanApproval, HarnessStage::Execution,
                            HarnessStage::MemoryUpdate, HarnessStage::Operations})
        required(succeeded(stage), "stage_not_succeeded:" + harness_stage_name(stage));
    const auto assurance_closed = std::any_of(
        checkpoint.stage_records.begin(), checkpoint.stage_records.end(),
        [](const auto& record) {
            return record.stage == HarnessStage::Assurance &&
                   (record.outcome == StageOutcome::Succeeded ||
                    record.outcome == StageOutcome::NeedsRemediation);
        });
    required(assurance_closed, "assurance_not_closed");
    if(checkpoint.judge_required)
        required(succeeded(HarnessStage::Judge), "stage_not_succeeded:judge");
    if(checkpoint.remediation_cycle > 0) {
        const auto finding_triggered = std::any_of(
            checkpoint.stage_records.begin(), checkpoint.stage_records.end(),
            [](const auto& record) {
                return (record.stage == HarnessStage::Assurance ||
                        record.stage == HarnessStage::Reverification) &&
                       record.outcome == StageOutcome::NeedsRemediation &&
                       !record.finding_ids.empty();
            });
        required(finding_triggered, "remediation_without_finding");
        required(succeeded(HarnessStage::Remediation), "stage_not_succeeded:remediation");
        required(succeeded(HarnessStage::Reexecution), "stage_not_succeeded:reexecution");
        required(succeeded(HarnessStage::Reverification),
                 "stage_not_succeeded:reverification");
    }
    return issues;
}

Phase4OperationsSnapshot Phase4HarnessRuntime::project_operations(
    const HarnessCheckpoint& checkpoint) {
    Phase4OperationsSnapshot snapshot;
    snapshot.snapshot_id = "harness:" + checkpoint.harness_id + ":r" +
                           std::to_string(checkpoint.revision);
    snapshot.tenant_id = checkpoint.metadata.identity.tenant_id;
    snapshot.run_id = checkpoint.metadata.identity.run_id;
    snapshot.task_id = checkpoint.metadata.identity.task_id;
    snapshot.updated_at = checkpoint.updated_at;
    snapshot.plan_revision = checkpoint.remediation_cycle + 1;
    snapshot.summary = "Phase 4 harness " + harness_state_name(checkpoint.state);
    snapshot.blocker = checkpoint.state == HarnessState::ManualReview ||
                               checkpoint.state == HarnessState::Failed
        ? checkpoint.terminal_reason : std::string();
    snapshot.residual_risk = checkpoint.unresolved_findings.empty()
        ? std::string() : "unresolved professional findings remain";
    switch(checkpoint.state) {
        case HarnessState::Completed: snapshot.overall_status = OperationsStatus::Passed; break;
        case HarnessState::Running: snapshot.overall_status = OperationsStatus::Running; break;
        case HarnessState::AwaitingApproval: snapshot.overall_status = OperationsStatus::Pending; break;
        case HarnessState::ManualReview: snapshot.overall_status = OperationsStatus::Blocked; break;
        case HarnessState::Rejected:
        case HarnessState::Failed: snapshot.overall_status = OperationsStatus::Failed; break;
        case HarnessState::Cancelled: snapshot.overall_status = OperationsStatus::Blocked; break;
    }
    std::set<std::string> evidence_ids;
    for(const auto& record : checkpoint.stage_records) {
        for(const auto& finding_id : record.finding_ids) {
            if(!evidence_ids.insert(finding_id).second) continue;
            OperationsEvidence evidence;
            evidence.id = finding_id;
            evidence.claim = "professional finding emitted by " +
                             harness_stage_name(record.stage);
            evidence.kind = "finding";
            evidence.source = "phase4-harness";
            evidence.authority = "advisory";
            evidence.freshness = checkpoint.updated_at;
            evidence.status = record.outcome == StageOutcome::NeedsRemediation
                ? OperationsStatus::Failed : operation_status(record.outcome);
            evidence.digest = record.output_digest;
            snapshot.evidence.push_back(std::move(evidence));
        }
        OperationsStage stage;
        stage.id = harness_stage_name(record.stage) + ":" + std::to_string(record.attempt);
        stage.label = harness_stage_name(record.stage);
        stage.status = operation_status(record.outcome);
        stage.revision = record.attempt;
        stage.role = "phase4-harness";
        stage.summary = record.error_message.empty()
            ? stage_outcome_name(record.outcome) : record.error_message;
        stage.evidence_ids = record.finding_ids;
        snapshot.stages.push_back(std::move(stage));
    }
    snapshot.unknowns = checkpoint.unresolved_findings;
    return snapshot;
}

}  // namespace agent_framework::harness
