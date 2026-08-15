#include "agent/harness/task_closure.hpp"

#include <algorithm>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <sqlite3.h>
#include "agent/contracts/contract.hpp"
#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::harness
{
    namespace
    {
        template <class T>
        std::set<T> setof(const std::vector<T> &v) { return {v.begin(), v.end()}; }
        std::size_t added(const std::vector<std::string> &a, const std::vector<std::string> &b)
        {
            auto old = setof(a);
            return std::count_if(b.begin(), b.end(), [&](const auto &v)
                                 { return !old.count(v); });
        }
        TaskClosureDecision decision(TaskTerminalState state, std::string reason, const TaskClosureContract &c, const ClosureFacts &f, std::vector<std::string> unsatisfied, std::string next)
        {
            TaskClosureDecision d;
            d.state = state;
            d.reason_code = std::move(reason);
            d.acceptance_contract_digest = contracts::canonical_digest(encode(c)).value_or("");
            d.unsatisfied_criteria = std::move(unsatisfied);
            d.artifact_refs = f.artifact_refs;
            d.evidence_refs = f.strong_evidence_refs;
            d.finding_refs = f.finding_refs;
            d.last_progress_revision = f.last_progress_revision;
            d.limitations = f.limitations;
            d.recommended_next_action = std::move(next);
            d.receipt_digest = contracts::canonical_digest({{"schema", "agent.task_closure_decision/v1"}, {"state", task_terminal_state_name(d.state)}, {"reason_code", d.reason_code}, {"contract_digest", d.acceptance_contract_digest}, {"unsatisfied", d.unsatisfied_criteria}, {"artifacts", d.artifact_refs}, {"evidence", d.evidence_refs}, {"findings", d.finding_refs}, {"next", d.recommended_next_action}}).value_or("");
            return d;
        }
        nlohmann::json assessment_json(const ProgressAssessment &a)
        {
            return {{"score",a.score},{"information_gain",a.information_gain},
                    {"consecutive_no_progress",a.consecutive_no_progress},{"digest",a.digest}};
        }
        std::string record_digest(std::string_view tenant, std::string_view task,
                                  const nlohmann::json &observation,
                                  const nlohmann::json &assessment)
        {
            return contracts::canonical_digest({{"schema","agent.progress_record/v1"},
                {"tenant_id",tenant},{"task_id",task},{"observation",observation},
                {"assessment",assessment}}).value_or("");
        }
        nlohmann::json progress_json(const ProgressObservation &v)
        {
            return {{"observation_id", v.observation_id}, {"revision", v.revision},
                    {"closed_criteria", v.closed_criteria},
                    {"valid_evidence_digests", v.valid_evidence_digests},
                    {"artifact_digests", v.artifact_digests},
                    {"resolved_findings", v.resolved_findings},
                    {"active_findings", v.active_findings}, {"blockers", v.blockers},
                    {"semantic_plan_digest", v.semantic_plan_digest},
                    {"input_tokens", v.input_tokens}, {"output_tokens", v.output_tokens},
                    {"tool_calls", v.tool_calls}, {"cost_usd", v.cost_usd}};
        }
        ProgressObservation progress_from(const nlohmann::json &j)
        {
            return {j.at("observation_id"), j.at("revision"), j.at("closed_criteria"),
                    j.at("valid_evidence_digests"), j.at("artifact_digests"),
                    j.at("resolved_findings"), j.at("active_findings"), j.at("blockers"),
                    j.at("semantic_plan_digest"), j.at("input_tokens"),
                    j.at("output_tokens"), j.at("tool_calls"), j.at("cost_usd")};
        }
    }
    std::string task_terminal_state_name(TaskTerminalState s)
    {
        switch (s)
        {
        case TaskTerminalState::Running:
            return "running";
        case TaskTerminalState::MinimalRemediation:
            return "minimal_remediation";
        case TaskTerminalState::CompletedVerified:
            return "completed_verified";
        case TaskTerminalState::CompletedWithLimitations:
            return "completed_with_limitations";
        case TaskTerminalState::NeedsUserInput:
            return "needs_user_input";
        case TaskTerminalState::BlockedExternal:
            return "blocked_external";
        case TaskTerminalState::BudgetExhausted:
            return "budget_exhausted";
        case TaskTerminalState::Stagnated:
            return "stagnated";
        case TaskTerminalState::FailedExecution:
            return "failed_execution";
        case TaskTerminalState::FailedVerification:
            return "failed_verification";
        case TaskTerminalState::ManualReview:
            return "manual_review";
        case TaskTerminalState::Cancelled:
            return "cancelled";
        }
        return "manual_review";
    }
    nlohmann::json encode(const TaskClosureContract &c)
    {
        const auto &i = c.metadata.identity;
        return {{"schema", "agent.task_closure_contract/v1"}, {"identity", {{"tenant_id", i.tenant_id}, {"organization_id", i.organization_id}, {"principal_id", i.principal_id}, {"project_id", i.project_id}, {"task_id", i.task_id}, {"run_id", i.run_id}, {"plan_id", i.plan_id}}}, {"contract_id", c.contract_id}, {"revision", c.revision}, {"task_class", c.task_class}, {"deliverables", c.deliverables}, {"mandatory_criteria", c.mandatory_criteria}, {"verification_methods", c.verification_methods}, {"allowed_side_effects", c.allowed_side_effects}, {"clarification_policy", c.clarification_policy}, {"max_iterations", c.max_iterations}, {"max_remediation_cycles", c.max_remediation_cycles}, {"max_no_progress_rounds", c.max_no_progress_rounds}, {"allow_limited_completion", c.allow_limited_completion}};
    }
    nlohmann::json encode(const TaskClosureDecision &d) { return {{"schema", "agent.task_closure_decision/v1"}, {"state", task_terminal_state_name(d.state)}, {"reason_code", d.reason_code}, {"terminal_authority", d.terminal_authority}, {"acceptance_contract_digest", d.acceptance_contract_digest}, {"unsatisfied_criteria", d.unsatisfied_criteria}, {"artifact_refs", d.artifact_refs}, {"evidence_refs", d.evidence_refs}, {"finding_refs", d.finding_refs}, {"last_progress_revision", d.last_progress_revision}, {"resume_token", d.resume_token}, {"recommended_next_action", d.recommended_next_action}, {"limitations", d.limitations}, {"receipt_digest", d.receipt_digest}}; }
    std::vector<std::string> validate(const TaskClosureContract &c)
    {
        std::vector<std::string> e;
        if (c.metadata.identity.tenant_id.empty() || c.metadata.identity.task_id.empty())
            e.push_back("closure_identity_required");
        if (c.contract_id.empty() || c.revision.empty() || c.task_class.empty())
            e.push_back("closure_contract_identity_required");
        if (c.mandatory_criteria.empty())
            e.push_back("mandatory_criteria_required");
        if (c.max_iterations == 0 || c.max_remediation_cycles == 0)
            e.push_back("positive_bounds_required");
        for (const auto &id : c.mandatory_criteria)
        {
            auto it = c.verification_methods.find(id);
            if (id.empty() || it == c.verification_methods.end() || it->second.empty())
                e.push_back("verification_method_required:" + id);
        }
        return e;
    }
    ProgressAssessment ProgressEvaluator::assess(const std::optional<ProgressObservation> &p, const ProgressObservation &c, std::uint64_t prior)
    {
        ProgressAssessment a;
        if (!p)
        {
            a.score = static_cast<std::int64_t>(c.closed_criteria.size() + c.valid_evidence_digests.size() + c.artifact_digests.size());
            a.information_gain = a.score > 0;
        }
        else
        {
            a.score = 3 * added(p->closed_criteria, c.closed_criteria) + 2 * added(p->valid_evidence_digests, c.valid_evidence_digests) + 2 * added(p->artifact_digests, c.artifact_digests) + added(p->resolved_findings, c.resolved_findings) - 2 * added(p->blockers, c.blockers) - static_cast<std::int64_t>(added(p->active_findings, c.active_findings));
            a.information_gain = a.score > 0;
        }
        a.consecutive_no_progress = a.information_gain ? 0 : prior + 1;
        a.digest = contracts::canonical_digest({{"schema", "agent.progress_assessment/v1"}, {"observation_id", c.observation_id}, {"revision", c.revision}, {"score", a.score}, {"information_gain", a.information_gain}, {"consecutive_no_progress", a.consecutive_no_progress}, {"plan_digest", c.semantic_plan_digest}}).value_or("");
        return a;
    }
    SQLiteProgressLedger::SQLiteProgressLedger(std::string path)
    {
        if (path.empty()) throw std::invalid_argument("progress ledger path required");
        std::filesystem::path file(path); std::error_code ec;
        if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
        sqlite3 *opened = nullptr;
        if (ec || sqlite3_open_v2(path.c_str(), &opened, SQLITE_OPEN_READWRITE |
                SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
            std::string error = opened ? sqlite3_errmsg(opened) : ec.message();
            if (opened) sqlite3_close(opened);
            throw std::runtime_error(error);
        }
        db_ = opened; sqlite3_busy_timeout(opened, 3000);
        internal::sqlite::exec(opened, "PRAGMA journal_mode=WAL");
        internal::sqlite::exec(opened, "PRAGMA synchronous=FULL");
        internal::sqlite::exec(opened, "CREATE TABLE IF NOT EXISTS phase4_progress("
            "tenant_id TEXT NOT NULL,task_id TEXT NOT NULL,revision INTEGER NOT NULL,"
            "observation_json TEXT NOT NULL,assessment_json TEXT NOT NULL,digest TEXT NOT NULL,"
            "PRIMARY KEY(tenant_id,task_id,revision))");
    }
    SQLiteProgressLedger::~SQLiteProgressLedger()
    { if (db_) sqlite3_close(internal::sqlite::database(db_)); }
    bool SQLiteProgressLedger::append(std::string_view tenant, std::string_view task,
        const ProgressObservation &o, const ProgressAssessment &a, std::string *error)
    {
        if (tenant.empty() || task.empty() || o.revision == 0 || o.observation_id.empty() ||
            a.digest.empty()) { if(error)*error="progress identity and digest required"; return false; }
        std::lock_guard lock(mutex_); auto *db=internal::sqlite::database(db_);
        internal::sqlite::Statement q(db,"INSERT INTO phase4_progress VALUES(?,?,?,?,?,?)");
        internal::sqlite::bind_text(q.get(),1,tenant); internal::sqlite::bind_text(q.get(),2,task);
        internal::sqlite::bind_int64(q.get(),3,o.revision);
        const auto observation=progress_json(o); const auto assessment=assessment_json(a);
        internal::sqlite::bind_text(q.get(),4,observation.dump());
        internal::sqlite::bind_text(q.get(),5,assessment.dump());
        internal::sqlite::bind_text(q.get(),6,record_digest(tenant,task,observation,assessment));
        if(internal::sqlite::step(q.get())!=SQLITE_DONE){if(error)*error=sqlite3_errmsg(db);return false;}
        return true;
    }
    std::optional<ProgressObservation> SQLiteProgressLedger::latest(
        std::string_view tenant,std::string_view task)
    {
        std::lock_guard lock(mutex_); auto*db=internal::sqlite::database(db_);
        internal::sqlite::Statement q(db,"SELECT observation_json,assessment_json,digest FROM phase4_progress WHERE "
            "tenant_id=? AND task_id=? ORDER BY revision DESC LIMIT 1");
        internal::sqlite::bind_text(q.get(),1,tenant);internal::sqlite::bind_text(q.get(),2,task);
        if(internal::sqlite::step(q.get())!=SQLITE_ROW)return std::nullopt;
        try{auto o=nlohmann::json::parse(internal::sqlite::column_text(q.get(),0));
            auto a=nlohmann::json::parse(internal::sqlite::column_text(q.get(),1));
            if(record_digest(tenant,task,o,a)!=internal::sqlite::column_text(q.get(),2))return std::nullopt;
            return progress_from(o);}
        catch(...){return std::nullopt;}
    }
    std::optional<ProgressAssessment> SQLiteProgressLedger::latest_assessment(
        std::string_view tenant,std::string_view task)
    {
        std::lock_guard lock(mutex_); auto*db=internal::sqlite::database(db_);
        internal::sqlite::Statement q(db,"SELECT observation_json,assessment_json,digest FROM phase4_progress WHERE "
            "tenant_id=? AND task_id=? ORDER BY revision DESC LIMIT 1");
        internal::sqlite::bind_text(q.get(),1,tenant);internal::sqlite::bind_text(q.get(),2,task);
        if(internal::sqlite::step(q.get())!=SQLITE_ROW)return std::nullopt;
        try{auto o=nlohmann::json::parse(internal::sqlite::column_text(q.get(),0));
            auto j=nlohmann::json::parse(internal::sqlite::column_text(q.get(),1));
            ProgressAssessment a{j.at("score"),j.at("information_gain"),
                j.at("consecutive_no_progress"),j.at("digest")};
            if(record_digest(tenant,task,o,j)!=internal::sqlite::column_text(q.get(),2))return std::nullopt;
            return a;}
        catch(...){return std::nullopt;}
    }
    TaskClosureDecision TaskClosureController::evaluate(const TaskClosureContract &c, const ClosureFacts &f) const
    {
        auto errors = validate(c);
        if (!errors.empty())
            return decision(TaskTerminalState::ManualReview, "closure_contract_invalid", c, f, c.mandatory_criteria, "repair contract");
        std::set<std::string> satisfied;
        for (const auto &verdict : f.criterion_verdicts)
        {
            const auto methods = c.verification_methods.find(verdict.criterion_id);
            const bool allowed_method = methods != c.verification_methods.end() &&
                std::find(methods->second.begin(), methods->second.end(),
                          verdict.verification_method) != methods->second.end();
            if (verdict.outcome == "pass" && allowed_method &&
                !verdict.evidence_refs.empty() && !verdict.artifact_refs.empty() &&
                !verdict.verifier_id.empty() && !verdict.report_digest.empty() &&
                verdict.revision == f.last_progress_revision)
                satisfied.insert(verdict.criterion_id);
        }
        std::vector<std::string> missing;
        for (const auto &id : c.mandatory_criteria)
            if (!satisfied.count(id))
                missing.push_back(id);
        if (f.cancelled)
            return decision(TaskTerminalState::Cancelled, "task_cancelled", c, f, missing, "none");
        if (f.unknown_side_effect || f.checkpoint.state == HarnessState::ManualReview)
            return decision(TaskTerminalState::ManualReview, "unknown_or_irreversible_effect", c, f, missing, "human review");
        if (!f.missing_facts.empty())
            return decision(TaskTerminalState::NeedsUserInput, "required_facts_missing", c, f, missing, "request missing facts");
        if (!f.external_blockers.empty())
            return decision(TaskTerminalState::BlockedExternal, "external_dependency_blocked", c, f, missing, "resume when dependency is available");
        if (f.budget_exhausted)
            return decision(TaskTerminalState::BudgetExhausted, "task_budget_exhausted", c, f, missing, "approve more budget or reduce scope");
        if (f.execution_failed)
            return decision(TaskTerminalState::FailedExecution, "execution_failed", c, f, missing, "inspect execution evidence");
        if (f.verification_failed && f.checkpoint.remediation_cycle >= c.max_remediation_cycles)
            return decision(TaskTerminalState::FailedVerification, "verification_failed_after_bound", c, f, missing, "human review");
        if (f.progress.consecutive_no_progress > c.max_no_progress_rounds)
            return decision(TaskTerminalState::Stagnated, "no_information_gain", c, f, missing, "request input or human review");
        if (!missing.empty() || f.verification_failed)
            return decision(TaskTerminalState::MinimalRemediation, "mandatory_criteria_unsatisfied", c, f, missing, "execute smallest approved remediation");
        const bool harness_complete = f.checkpoint.state == HarnessState::Completed && Phase4HarnessRuntime::completion_gate_issues(f.checkpoint).empty();
        if (!harness_complete)
            return decision(TaskTerminalState::Running, "harness_not_terminal", c, f, missing, "continue harness");
        if (f.strong_evidence_refs.empty() || f.artifact_refs.empty())
            return decision(TaskTerminalState::ManualReview, "strong_evidence_or_artifact_missing", c, f, missing, "collect production evidence");
        if (!f.limitations.empty()) {
            if (!c.allow_limited_completion)
                return decision(TaskTerminalState::ManualReview, "limitations_not_authorized", c, f, {}, "review limitations");
            return decision(TaskTerminalState::CompletedWithLimitations,
                "mandatory_criteria_verified_with_authorized_limitations", c, f, {}, "none");
        }
        return decision(TaskTerminalState::CompletedVerified, "all_mandatory_criteria_verified", c, f, {}, "none");
    }

    TaskRouteDecision ProductionTaskRouter::route(const ProductionTaskRoute &r)
    {
        if(!r.production) return {true,"legacy_demo",{}};
        TaskRouteDecision d; d.route="production_harness";
        const auto require=[&](bool ok,const char* name){if(!ok)d.missing_dependencies.emplace_back(name);};
        require(r.closure_contract,"task_closure_contract");
        require(r.production_composition,"default_production_composition");
        require(r.closure_controller,"task_closure_controller");
        require(r.coordination,"csac_coordinator");
        require(r.durable_stores,"durable_stores");
        require(r.dependency_manifest,"production_dependency_manifest");
        d.allowed=d.missing_dependencies.empty();
        if(!d.allowed)d.route="fail_closed";
        return d;
    }

    TaskClosureDecision ProductionTaskRuntime::start(const HarnessStart& start,
        const HarnessRuntimeOptions& options) { return close(harness_.run(start,options)); }
    TaskClosureDecision ProductionTaskRuntime::resume(std::string_view tenant,std::string_view id,
        const HarnessRuntimeOptions& options) { return close(harness_.resume(tenant,id,options)); }
    TaskClosureDecision ProductionTaskRuntime::close(const HarnessRunResult& run)
    {
        const auto& identity=contract_.metadata.identity;
        auto previous=progress_.latest(identity.tenant_id,identity.task_id);
        auto prior_assessment=progress_.latest_assessment(identity.tenant_id,identity.task_id);
        ProgressObservation current;
        current.observation_id=run.checkpoint.harness_id+":"+std::to_string(run.checkpoint.revision);
        current.revision=run.checkpoint.revision;
        current.active_findings=run.checkpoint.unresolved_findings;
        current.semantic_plan_digest=identity.plan_id;
        if(!run.checkpoint.pins.artifact_manifest_digest.empty())
            current.artifact_digests.push_back(run.checkpoint.pins.artifact_manifest_digest);
        if(!run.checkpoint.pins.acceptance_report_digest.empty())
            current.valid_evidence_digests.push_back(run.checkpoint.pins.acceptance_report_digest);
        if(!run.checkpoint.pins.judge_report_digest.empty())
            current.valid_evidence_digests.push_back(run.checkpoint.pins.judge_report_digest);
        // Harness structural completion cannot close semantic criteria.  A
        // production assurance/judge adapter must supply typed criterion
        // verdicts to TaskClosureController.
        ProgressAssessment assessment;
        if(previous && previous->revision==current.revision && prior_assessment) {
            current=*previous;
            assessment=*prior_assessment;
        } else if(current.revision>0) {
            assessment=ProgressEvaluator::assess(previous,current,
                prior_assessment ? prior_assessment->consecutive_no_progress : 0);
            std::string ignored;
            progress_.append(identity.tenant_id,identity.task_id,current,assessment,&ignored);
        }
        ClosureFacts facts; facts.checkpoint=run.checkpoint;
        facts.cancelled=run.checkpoint.state==HarnessState::Cancelled;
        facts.execution_failed=run.checkpoint.state==HarnessState::Failed;
        facts.unknown_side_effect=run.checkpoint.state==HarnessState::ManualReview;
        facts.verification_failed=run.error_code=="verification_failed";
        facts.last_progress_revision=run.checkpoint.revision;
        facts.finding_refs=run.checkpoint.unresolved_findings;
        if(verdicts_) facts.criterion_verdicts=verdicts_(run.checkpoint);
        for(const auto& verdict:facts.criterion_verdicts)
            if(verdict.outcome=="pass") current.closed_criteria.push_back(verdict.criterion_id);
        facts.strong_evidence_refs=current.valid_evidence_digests;
        facts.artifact_refs=current.artifact_digests;
        facts.progress=assessment;
        return closure_.evaluate(contract_,facts);
    }
} // namespace agent_framework::harness
