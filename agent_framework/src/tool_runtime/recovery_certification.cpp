#include "agent/tool_runtime/recovery_certification.hpp"
#include "agent/contracts/contract.hpp"
#include <random>
#include <fstream>
namespace agent_framework::tool_runtime {
std::string_view name(RecoveryEvidenceLevel v){static constexpr std::string_view n[]={"offline","process_live","provider_live"};return n[static_cast<int>(v)];}
std::string_view name(RecoveryScenario v){static constexpr std::string_view n[]={"worker_crash_takeover","stale_completion","cancel_before_start","cancel_complete_race","deadline_result_race","cancel_restart_escalation","object_corruption","missing_object","sqlite_busy","storage_failure","disk_write_failure","schema_migration","schema_incompatible","receipt_loss_reconciliation","client_reconnect","provider_disconnect","provider_reattach","mcp_disconnect","mcp_late_result"};return n[static_cast<int>(v)];}
std::vector<RecoveryScenario> mandatory_recovery_scenarios(){return {RecoveryScenario::WorkerCrashTakeover,RecoveryScenario::StaleCompletion,RecoveryScenario::CancelBeforeStart,RecoveryScenario::CancelCompleteRace,RecoveryScenario::DeadlineResultRace,RecoveryScenario::CancelRestartEscalation,RecoveryScenario::ObjectCorruption,RecoveryScenario::MissingObject,RecoveryScenario::SqliteBusy,RecoveryScenario::StorageFailure,RecoveryScenario::DiskWriteFailure,RecoveryScenario::SchemaMigration,RecoveryScenario::SchemaIncompatible,RecoveryScenario::ReceiptLossReconciliation,RecoveryScenario::ClientReconnect,RecoveryScenario::ProviderDisconnect,RecoveryScenario::ProviderReattach,RecoveryScenario::McpDisconnect,RecoveryScenario::McpLateResult};}
namespace { RecoveryEvidenceLevel required_level(RecoveryScenario scenario){switch(scenario){case RecoveryScenario::WorkerCrashTakeover:case RecoveryScenario::ClientReconnect:return RecoveryEvidenceLevel::ProcessLive;case RecoveryScenario::ProviderDisconnect:case RecoveryScenario::ProviderReattach:case RecoveryScenario::McpDisconnect:case RecoveryScenario::McpLateResult:return RecoveryEvidenceLevel::ProviderLive;default:return RecoveryEvidenceLevel::Offline;}} }
RecoveryCertificationReport certify_recovery(std::string environment,RecoveryEvidenceLevel available,const RecoveryScenarioExecutor& execute){RecoveryCertificationReport r;r.environment_digest=std::move(environment);for(auto scenario:mandatory_recovery_scenarios()){RecoveryCell c;c.scenario=scenario;c.required_level=required_level(scenario);if(static_cast<int>(available)<static_cast<int>(c.required_level)){c.reason="required evidence level unavailable";}else if(execute)c=execute(scenario);c.scenario=scenario;c.required_level=required_level(scenario);if(!c.executed)r.blockers.push_back(std::string(name(scenario))+":required_not_executed");else if(!c.passed)r.blockers.push_back(std::string(name(scenario))+":failed:"+c.reason);else if(c.evidence_digest.empty())r.blockers.push_back(std::string(name(scenario))+":missing_evidence");r.cells.push_back(std::move(c));}if(r.environment_digest.empty())r.blockers.push_back("environment_digest_missing");r.certified=r.blockers.empty();return r;}
nlohmann::json encode(const RecoveryCertificationReport&r){nlohmann::json cells=nlohmann::json::array();for(const auto&c:r.cells)cells.push_back({{"scenario",name(c.scenario)},{"required_level",name(c.required_level)},{"required",c.required},{"executed",c.executed},{"passed",c.passed},{"reason",c.reason},{"evidence_digest",c.evidence_digest},{"operations",c.operations},{"failures",c.failures}});return {{"matrix_revision",r.matrix_revision},{"environment_digest",r.environment_digest},{"cells",cells},{"blockers",r.blockers},{"certified",r.certified}};}
SoakReport run_recovery_soak(const SoakOptions&o,const SoakOperation&fn){SoakReport r;r.iterations=o.iterations;r.seed=o.seed;r.concurrency=o.concurrency;if(!o.iterations||!o.concurrency||!fn){r.failures=1;return r;}std::mt19937_64 random(o.seed);for(std::uint64_t i=0;i<o.iterations;i++){r.operations++;if(fn(i,random()))r.recoveries++;else r.failures++;}r.passed=r.failures==0;auto value=nlohmann::json{{"iterations",r.iterations},{"operations",r.operations},{"recoveries",r.recoveries},{"failures",r.failures},{"seed",r.seed},{"concurrency",r.concurrency},{"passed",r.passed}};r.evidence_digest=contracts::canonical_digest(value).value_or("");return r;}
LongTaskMetricsGate certify_long_task_metrics(const LongTaskOperationalMetrics&m){
    LongTaskMetricsGate gate;gate.metrics=m;
    if(m.orphan_running)gate.blockers.push_back("orphan_running_nonzero");
    if(m.state_divergence)gate.blockers.push_back("state_divergence_nonzero");
    if(m.duplicate_effects)gate.blockers.push_back("duplicate_effect_nonzero");
    if(m.empty_completed)gate.blockers.push_back("empty_completed_nonzero");
    if(!m.resume_attempts)gate.blockers.push_back("resume_sample_missing");
    else if(m.resume_successes>m.resume_attempts||
            m.resume_successes*10000<m.resume_attempts*9900)
        gate.blockers.push_back("resume_success_below_99_percent");
    if(!m.first_progress_p95_ms||m.first_progress_p95_ms>10000)
        gate.blockers.push_back("first_progress_p95_exceeds_10s");
    if(m.heartbeat_interval_p95_ms<5000||m.heartbeat_interval_p95_ms>15000)
        gate.blockers.push_back("heartbeat_p95_outside_5_15s");
    gate.passed=gate.blockers.empty();
    const auto document=nlohmann::json{{"orphan_running",m.orphan_running},
        {"state_divergence",m.state_divergence},{"duplicate_effects",m.duplicate_effects},
        {"empty_completed",m.empty_completed},{"resume_attempts",m.resume_attempts},
        {"resume_successes",m.resume_successes},{"first_progress_p95_ms",m.first_progress_p95_ms},
        {"heartbeat_interval_p95_ms",m.heartbeat_interval_p95_ms},
        {"blockers",gate.blockers},{"passed",gate.passed}};
    gate.evidence_digest=contracts::canonical_digest(document).value_or("");
    return gate;
}
RecoveryCertificationReport run_provider_live_certification(const ProviderLiveOptions&o,const RecoveryScenarioExecutor&executor,const SoakOperation&operation,std::string*error){auto report=certify_recovery(o.environment_digest,RecoveryEvidenceLevel::ProviderLive,executor);auto soak=run_recovery_soak({o.iterations,o.seed,o.concurrency},operation);if(!soak.passed){report.certified=false;report.blockers.push_back("provider_soak_failed_or_not_executed");}if(o.output_path.empty()){report.certified=false;report.blockers.push_back("provider_live_report_path_missing");}else{std::ofstream out(o.output_path,std::ios::binary|std::ios::trunc);if(!out){report.certified=false;report.blockers.push_back("provider_live_report_write_failed");if(error)*error="unable to write provider live report";}else{auto document=encode(report);document["soak"]={{"iterations",soak.iterations},{"operations",soak.operations},{"recoveries",soak.recoveries},{"failures",soak.failures},{"seed",soak.seed},{"concurrency",soak.concurrency},{"evidence_digest",soak.evidence_digest},{"passed",soak.passed}};out<<document.dump(2);}}return report;}
}
