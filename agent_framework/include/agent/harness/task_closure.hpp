#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "agent/harness/runtime.hpp"

namespace agent_framework::harness {

enum class TaskTerminalState { Running, MinimalRemediation, CompletedVerified,
    CompletedWithLimitations, NeedsUserInput, BlockedExternal, BudgetExhausted,
    Stagnated, FailedExecution, FailedVerification, ManualReview, Cancelled };

struct TaskClosureContract {
    contracts::ContractMetadata metadata;
    std::string contract_id;
    std::string revision;
    std::string task_class;
    std::vector<std::string> deliverables;
    std::vector<std::string> mandatory_criteria;
    std::map<std::string,std::vector<std::string>> verification_methods;
    std::vector<std::string> allowed_side_effects;
    std::string clarification_policy;
    std::uint64_t max_iterations{10};
    std::uint64_t max_remediation_cycles{2};
    std::uint64_t max_no_progress_rounds{1};
    bool allow_limited_completion{false};
};

struct ProgressObservation {
    std::string observation_id;
    std::uint64_t revision{0};
    std::vector<std::string> closed_criteria;
    std::vector<std::string> valid_evidence_digests;
    std::vector<std::string> artifact_digests;
    std::vector<std::string> resolved_findings;
    std::vector<std::string> active_findings;
    std::vector<std::string> blockers;
    std::string semantic_plan_digest;
    std::uint64_t input_tokens{0}, output_tokens{0}, tool_calls{0};
    double cost_usd{0.0};
};

struct ProgressAssessment {
    std::int64_t score{0};
    bool information_gain{false};
    std::uint64_t consecutive_no_progress{0};
    std::string digest;
};

struct ClosureFacts {
    struct CriterionVerdict {
        std::string criterion_id;
        std::string outcome;
        std::vector<std::string> evidence_refs;
        std::vector<std::string> artifact_refs;
        std::string verification_method;
        std::string verifier_id;
        std::string report_digest;
        std::uint64_t revision{0};
    };
    HarnessCheckpoint checkpoint;
    // Deprecated compatibility input. It is deliberately ignored by the
    // closure controller because a list of identifiers is not evidence.
    std::vector<std::string> satisfied_criteria;
    std::vector<CriterionVerdict> criterion_verdicts;
    std::vector<std::string> strong_evidence_refs;
    std::vector<std::string> artifact_refs;
    std::vector<std::string> finding_refs;
    std::vector<std::string> missing_facts;
    std::vector<std::string> external_blockers;
    ProgressAssessment progress;
    std::uint64_t last_progress_revision{0};
    std::vector<std::string> limitations;
    bool cancelled{false};
    bool budget_exhausted{false};
    bool execution_failed{false};
    bool verification_failed{false};
    bool unknown_side_effect{false};
};

struct ProductionTaskRoute {
    bool production{true};
    bool closure_contract{false};
    bool production_composition{false};
    bool closure_controller{false};
    bool coordination{false};
    bool durable_stores{false};
    bool dependency_manifest{false};
};

struct TaskRouteDecision {
    bool allowed{false};
    std::string route;
    std::vector<std::string> missing_dependencies;
};

class ProductionTaskRouter {
public:
    static TaskRouteDecision route(const ProductionTaskRoute& request);
};

struct TaskClosureDecision {
    TaskTerminalState state{TaskTerminalState::Running};
    std::string reason_code;
    std::string terminal_authority{"task_closure_controller"};
    std::string acceptance_contract_digest;
    std::vector<std::string> unsatisfied_criteria;
    std::vector<std::string> artifact_refs, evidence_refs, finding_refs;
    std::uint64_t last_progress_revision{0};
    std::string resume_token;
    std::string recommended_next_action;
    std::vector<std::string> limitations;
    std::string receipt_digest;
};

std::string task_terminal_state_name(TaskTerminalState);
nlohmann::json encode(const TaskClosureContract&);
nlohmann::json encode(const TaskClosureDecision&);
std::vector<std::string> validate(const TaskClosureContract&);

class ProgressEvaluator {
public:
    static ProgressAssessment assess(const std::optional<ProgressObservation>& previous,
                                     const ProgressObservation& current,
                                     std::uint64_t previous_no_progress = 0);
};

class SQLiteProgressLedger {
public:
    explicit SQLiteProgressLedger(std::string path);
    ~SQLiteProgressLedger();
    SQLiteProgressLedger(const SQLiteProgressLedger&) = delete;
    SQLiteProgressLedger& operator=(const SQLiteProgressLedger&) = delete;
    bool append(std::string_view tenant_id, std::string_view task_id,
                const ProgressObservation&, const ProgressAssessment&,
                std::string* error = nullptr);
    std::optional<ProgressObservation> latest(std::string_view tenant_id,
                                               std::string_view task_id);
    std::optional<ProgressAssessment> latest_assessment(std::string_view tenant_id,
                                                         std::string_view task_id);
private:
    void* db_{nullptr};
    std::mutex mutex_;
};

class TaskClosureController {
public:
    TaskClosureDecision evaluate(const TaskClosureContract& contract,
                                 const ClosureFacts& facts) const;
};

class ProductionTaskRuntime {
public:
    using CriterionVerdictProvider = std::function<std::vector<ClosureFacts::CriterionVerdict>(
        const HarnessCheckpoint&)>;
    ProductionTaskRuntime(TaskClosureContract contract, Phase4HarnessRuntime& harness,
                          SQLiteProgressLedger& progress, TaskClosureController& closure,
                          CriterionVerdictProvider verdicts = {})
        : contract_(std::move(contract)), harness_(harness), progress_(progress), closure_(closure),
          verdicts_(std::move(verdicts)) {}
    TaskClosureDecision start(const HarnessStart&, const HarnessRuntimeOptions& = {});
    TaskClosureDecision resume(std::string_view tenant_id, std::string_view harness_id,
                               const HarnessRuntimeOptions& = {});
private:
    TaskClosureDecision close(const HarnessRunResult&);
    TaskClosureContract contract_;
    Phase4HarnessRuntime& harness_;
    SQLiteProgressLedger& progress_;
    TaskClosureController& closure_;
    CriterionVerdictProvider verdicts_;
};

} // namespace agent_framework::harness
