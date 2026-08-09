#include <cassert>
#include <fstream>
#include <string>
#include <vector>

#include "agent/approval/types.hpp"
#include "agent/assurance/types.hpp"
#include "agent/contracts/contract.hpp"
#include "agent/eval/types.hpp"
#include "agent/memory_v2/types.hpp"
#include "agent/planning/types.hpp"
#include "agent/run/types.hpp"
#include "agent/sandbox/types.hpp"
#include "agent/telemetry/types.hpp"

#ifndef AGENT_PHASE4_FIXTURE_ROOT
#define AGENT_PHASE4_FIXTURE_ROOT "tests/fixtures/phase4"
#endif

namespace {
using namespace agent_framework;
using json = nlohmann::json;

contracts::ContractMetadata metadata(bool task = true) {
    contracts::ContractMetadata value;
    value.identity.tenant_id = "tenant-a";
    value.identity.organization_id = "org-a";
    value.identity.principal_id = "user-a";
    value.identity.project_id = "project-a";
    if(task) value.identity.task_id = "task-a";
    value.identity.run_id = "run-a";
    value.identity.plan_id = "plan-a";
    return value;
}

json load(const std::string& name) {
    std::ifstream input(std::string(AGENT_PHASE4_FIXTURE_ROOT) + "/contracts/" + name);
    assert(input.good());
    json value;
    input >> value;
    return value;
}

void common_contract_guards() {
    planning::TaskIntake intake;
    intake.metadata = metadata();
    intake.user_goal = "implement durable planning";
    intake.requested_deliverables = {"code", "tests"};
    auto encoded = planning::encode(intake);
    assert(encoded.at("schema_version") == 1);
    assert(encoded.at("canonical_digest").get<std::string>().starts_with("sha256:"));
    assert(planning::decode_task_intake(encoded));

    auto tampered = encoded;
    tampered["payload"]["user_goal"] = "tampered";
    std::vector<contracts::ContractIssue> issues;
    assert(!planning::decode_task_intake(tampered, {}, &issues));
    assert(!issues.empty() && issues.back().code == "digest_mismatch");

    auto unknown = encoded;
    unknown["surprise"] = true;
    unknown["canonical_digest"] = *contracts::embedded_digest(unknown);
    issues.clear();
    assert(!planning::decode_task_intake(unknown, {}, &issues));

    auto redacted = contracts::redact_json(encoded, {{"/payload/user_goal", "[MASKED]"}});
    assert(redacted.at("payload").at("user_goal") == "[MASKED]");
    assert(contracts::verify_embedded_digest(redacted));

    contracts::SchemaMigrator migrator;
    assert(migrator.register_step(0, 1, [](const json& legacy) {
        auto migrated = legacy;
        migrated["payload"]["user_goal"] = migrated["payload"].at("goal");
        migrated["payload"].erase("goal");
        return migrated;
    }));
    auto migrated = migrator.migrate(load("task_intake_v0.json"), 1, &issues);
    assert(migrated && planning::decode_task_intake(*migrated));
    issues.clear();
    assert(!contracts::parse_typed_contract(load("future_schema.json"),
        "agent.task_intake/v1", {}, &issues));
}

void domain_round_trips() {
    planning::EvidenceBundle evidence;
    evidence.metadata = metadata();
    evidence.bundle_id = "evidence-a";
    evidence.records.push_back({"ev-1", "repository", "src/main.cpp", "sha256:a", "now",
                                "verified", "later", {"claim-1"}, {}, false});
    assert(planning::decode_evidence_bundle(planning::encode(evidence)));

    planning::TaskUnderstanding understanding;
    understanding.metadata = metadata();
    understanding.domain = "software";
    understanding.change_mode = planning::ChangeMode::Upgrade;
    assert(planning::decode_task_understanding(planning::encode(understanding)));

    planning::ExecutionPlan plan;
    plan.metadata = metadata();
    plan.nodes.push_back({"node-1", "build contracts", {}, {}, {}, {}, {}, {}, {},
                          "acceptance-a", "revert commit", "low", false});
    assert(planning::decode_execution_plan(planning::encode(plan)));

    assurance::AcceptanceContract acceptance;
    acceptance.metadata = metadata();
    acceptance.criteria.push_back({"c-1", assurance::VerificationLayer::Module,
        "round-trip", "deterministic", {"test"}, "pass", true});
    assert(assurance::decode_acceptance_contract(assurance::encode(acceptance)));

    assurance::AcceptanceReport report;
    report.metadata = metadata();
    report.findings.push_back({"f-1", "c-1", "info", assurance::FindingOutcome::Pass,
                               1.0, {"ev-1"}, ""});
    report.decision = assurance::AcceptanceDecision::Accepted;
    assert(assurance::decode_acceptance_report(assurance::encode(report)));

    run::RunCheckpoint checkpoint;
    checkpoint.metadata = metadata();
    checkpoint.state = run::RunState::Running;
    assert(run::decode_run_checkpoint(run::encode(checkpoint)));
    run::Interruption interruption;
    interruption.metadata = metadata();
    interruption.interruption_id = "interrupt-a";
    assert(run::decode_interruption(run::encode(interruption)));

    approval::ApprovalDecision decision;
    decision.metadata = metadata();
    decision.approval_id = "approval-a";
    decision.decision = approval::Decision::Approved;
    assert(approval::decode_approval_decision(approval::encode(decision)));
    approval::ApprovalRequest request;
    request.metadata = metadata();
    request.approval_id = "approval-a";
    request.request_kind = "plan";
    assert(approval::decode_approval_request(approval::encode(request)));

    memory_v2::MemoryRecord record;
    record.metadata = metadata(false);
    record.record_id = "memory-a";
    record.scope.tenant_id = "tenant-a";
    record.scope.level = memory_v2::MemoryLevel::Project;
    record.authority = memory_v2::Authority::Verified;
    assert(memory_v2::decode_memory_record(memory_v2::encode(record)));
    memory_v2::MemorySnapshot snapshot;
    snapshot.metadata = metadata(false);
    snapshot.snapshot_id = "snapshot-a";
    assert(memory_v2::decode_memory_snapshot(memory_v2::encode(snapshot)));
    memory_v2::MemoryViewSpec view_spec;
    view_spec.metadata = metadata(false);
    view_spec.allowed_levels = {memory_v2::MemoryLevel::System, memory_v2::MemoryLevel::Task};
    assert(memory_v2::decode_memory_view_spec(memory_v2::encode(view_spec)));
    memory_v2::MemoryViewManifest manifest;
    manifest.metadata = metadata(false);
    manifest.snapshot_id = "snapshot-a";
    assert(memory_v2::decode_memory_view_manifest(memory_v2::encode(manifest)));
    memory_v2::MemoryConflict conflict;
    conflict.metadata = metadata(false);
    conflict.conflict_id = "conflict-a";
    assert(memory_v2::decode_memory_conflict(memory_v2::encode(conflict)));

    sandbox::SandboxSpec spec;
    spec.metadata = metadata();
    spec.provider = "process";
    assert(sandbox::decode_sandbox_spec(sandbox::encode(spec)));
    sandbox::SandboxManifest sandbox_manifest;
    sandbox_manifest.metadata = metadata();
    sandbox_manifest.sandbox_id = "sandbox-a";
    assert(sandbox::decode_sandbox_manifest(sandbox::encode(sandbox_manifest)));

    telemetry::CorrelationContext correlation;
    correlation.metadata = metadata();
    correlation.trace_id = "trace-a";
    assert(telemetry::decode_correlation_context(telemetry::encode(correlation)));
    telemetry::MetricResult metric;
    metric.metadata = metadata();
    metric.metric_name = "success_rate";
    assert(telemetry::decode_metric_result(telemetry::encode(metric)));

    eval::DatasetCase dataset_case;
    dataset_case.metadata = metadata();
    dataset_case.case_id = "case-a";
    assert(eval::decode_dataset_case(eval::encode(dataset_case)));
    eval::Trajectory trajectory;
    trajectory.metadata = metadata();
    trajectory.case_id = "case-a";
    assert(eval::decode_trajectory(eval::encode(trajectory)));
    eval::ComparisonResult comparison;
    comparison.metadata = metadata();
    comparison.baseline_version = "v1";
    comparison.candidate_version = "v2";
    assert(eval::decode_comparison_result(eval::encode(comparison)));
}
}  // namespace

int main() {
    common_contract_guards();
    domain_round_trips();
    return 0;
}
