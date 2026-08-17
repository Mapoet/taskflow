#include "agent/recovery/task_state_coordinator.hpp"

#include "agent/contracts/contract.hpp"
#include "agent/internal/sqlite_utils.hpp"
#include "agent/run/state_machine.hpp"

#include <filesystem>
#include <sqlite3.h>
#include <stdexcept>

namespace agent_framework::recovery {
namespace {
bool run_terminal(run::RunState state) {
    return state == run::RunState::Completed || state == run::RunState::Partial ||
           state == run::RunState::Rejected || state == run::RunState::Failed ||
           state == run::RunState::Cancelled;
}

bool harness_terminal(harness::HarnessState state) {
    return state == harness::HarnessState::Completed ||
           state == harness::HarnessState::Rejected ||
           state == harness::HarnessState::Failed ||
           state == harness::HarnessState::Cancelled;
}

TaskCoordinationDecision make(const CorrelatedStateEvent& event,
                              CoordinationCommand command,
                              conversation::TaskLifecycleState state,
                              std::string closure, std::string reason,
                              bool terminal = false) {
    TaskCoordinationDecision decision{command, state, std::move(closure),
                                      std::move(reason), terminal, {}};
    decision.digest = contracts::canonical_digest({
        {"schema", "agent.task_coordination_decision/v1"},
        {"tenant_id", event.identity.tenant_id},
        {"conversation_id", event.identity.conversation_id},
        {"task_id", event.task_id}, {"turn_id", event.turn_id},
        {"run_id", event.run_id}, {"harness_id", event.harness_id},
        {"task_revision", event.task_revision}, {"command", name(command)},
        {"task_state", conversation::name(state)},
        {"closure_state", decision.closure_state},
        {"reason_code", decision.reason_code}, {"terminal", terminal},
        {"source_event_id", event.source_event_id}}).value_or("");
    return decision;
}
}  // namespace

std::string_view name(CoordinationCommand command) noexcept {
    switch(command) {
        case CoordinationCommand::None: return "none";
        case CoordinationCommand::ContinueExecution: return "continue_execution";
        case CoordinationCommand::AwaitInput: return "await_input";
        case CoordinationCommand::AwaitApproval: return "await_approval";
        case CoordinationCommand::AwaitExternal: return "await_external";
        case CoordinationCommand::VerifyCompletion: return "verify_completion";
        case CoordinationCommand::CloseVerified: return "close_verified";
        case CoordinationCommand::Fail: return "fail";
        case CoordinationCommand::Cancel: return "cancel";
        case CoordinationCommand::ManualReview: return "manual_review";
    }
    return "none";
}

TaskCoordinationDecision TaskStateCoordinator::observe(
    const CorrelatedStateEvent& event) const {
    if(event.identity.tenant_id.empty() || event.identity.conversation_id.empty() ||
       event.task_id.empty() || event.turn_id.empty() || event.run_id.empty() ||
       event.harness_id.empty() || event.task_revision == 0)
        return make(event, CoordinationCommand::ManualReview,
            conversation::TaskLifecycleState::Failed, "coordination_contract_invalid",
            "coordination_contract_invalid");
    if(event.unknown_effect)
        return make(event, CoordinationCommand::ManualReview,
            conversation::TaskLifecycleState::Suspended, "manual_review",
            "unknown_effect_requires_reconciliation");
    if(event.run_state == run::RunState::Cancelled ||
       event.harness_state == harness::HarnessState::Cancelled)
        return make(event, CoordinationCommand::Cancel,
            conversation::TaskLifecycleState::Cancelled, "cancelled",
            "coordinated_cancellation", true);
    if(event.turn_phase == conversation::TurnPhase::AwaitingInput)
        return make(event, event.run_state == run::RunState::AwaitingApproval
                ? CoordinationCommand::AwaitApproval : CoordinationCommand::AwaitInput,
            event.run_state == run::RunState::AwaitingApproval
                ? conversation::TaskLifecycleState::AwaitingApproval
                : conversation::TaskLifecycleState::AwaitingInput,
            event.run_state == run::RunState::AwaitingApproval
                ? "awaiting_approval" : "awaiting_input",
            event.run_state == run::RunState::AwaitingApproval
                ? "durable_approval_required" : "durable_user_input_required");
    if(event.harness_state == harness::HarnessState::AwaitingExternal ||
       event.run_state == run::RunState::Waiting)
        return make(event, CoordinationCommand::AwaitExternal,
            conversation::TaskLifecycleState::Suspended, "awaiting_external",
            "durable_external_dependency_wait");

    const bool execution_done = event.run_state == run::RunState::Completed &&
                                event.harness_state == harness::HarnessState::Completed;
    const bool unsettled = event.invocation_active || event.pending_effect;
    if(execution_done && !unsettled && event.closure_verified &&
       event.turn_phase == conversation::TurnPhase::Completed)
        return make(event, CoordinationCommand::CloseVerified,
            conversation::TaskLifecycleState::Closed, "completed_verified",
            "all_mandatory_criteria_verified", true);
    if(execution_done)
        return make(event, unsettled ? CoordinationCommand::ContinueExecution
                                    : CoordinationCommand::VerifyCompletion,
            conversation::TaskLifecycleState::Closing,
            unsettled ? "settling_execution" : "execution_completed_unverified",
            unsettled ? "late_invocation_or_effect_pending"
                      : "semantic_verification_required");

    const bool execution_failed = event.run_state == run::RunState::Failed ||
        event.run_state == run::RunState::Rejected ||
        event.harness_state == harness::HarnessState::Failed ||
        event.harness_state == harness::HarnessState::Rejected;
    if(execution_failed && !event.invocation_active && !event.pending_effect &&
       run_terminal(event.run_state) && harness_terminal(event.harness_state))
        return make(event, CoordinationCommand::Fail,
            conversation::TaskLifecycleState::Failed, "failed_execution",
            "coordinated_execution_failure", true);

    return make(event, CoordinationCommand::ContinueExecution,
        conversation::TaskLifecycleState::Active, "running",
        event.turn_phase == conversation::TurnPhase::Failed
            ? "conversation_failed_but_execution_still_active"
            : "correlated_execution_nonterminal");
}

std::string_view name(TaskCoordinationCommandState state) noexcept {
    switch(state) {
        case TaskCoordinationCommandState::Pending: return "pending";
        case TaskCoordinationCommandState::Applied: return "applied";
        case TaskCoordinationCommandState::Conflict: return "conflict";
    }
    return "conflict";
}

std::string_view name(CoordinationBoundary boundary) noexcept {
    switch(boundary) {
        case CoordinationBoundary::Conversation: return "conversation";
        case CoordinationBoundary::Harness: return "harness";
        case CoordinationBoundary::Run: return "run";
        case CoordinationBoundary::Invocation: return "invocation";
        case CoordinationBoundary::Effect: return "effect";
        case CoordinationBoundary::Closure: return "closure";
    }
    return "unknown";
}

namespace {
using internal::sqlite::Statement;
TaskCoordinationCommandState command_state(std::string_view value) {
    if(value == "pending") return TaskCoordinationCommandState::Pending;
    if(value == "applied") return TaskCoordinationCommandState::Applied;
    return TaskCoordinationCommandState::Conflict;
}
nlohmann::json encode_event(const CorrelatedStateEvent& e) {
    return {{"tenant_id",e.identity.tenant_id},{"conversation_id",e.identity.conversation_id},
        {"task_id",e.task_id},{"turn_id",e.turn_id},{"run_id",e.run_id},
        {"harness_id",e.harness_id},{"task_revision",e.task_revision},
        {"turn_phase",conversation::name(e.turn_phase)},
        {"run_state",run::run_state_name(e.run_state)},
        {"harness_state",harness::harness_state_name(e.harness_state)},
        {"invocation_active",e.invocation_active},{"pending_effect",e.pending_effect},
        {"unknown_effect",e.unknown_effect},{"closure_verified",e.closure_verified},
        {"source_event_id",e.source_event_id}};
}
nlohmann::json encode_decision(const TaskCoordinationDecision& d) {
    return {{"command",name(d.command)},{"task_state",conversation::name(d.task_state)},
        {"closure_state",d.closure_state},{"reason_code",d.reason_code},
        {"terminal",d.terminal},{"digest",d.digest}};
}
std::optional<DurableTaskCoordinationCommand> decode_command(
    std::string_view id, std::string_view event_text, std::string_view decision_text,
    std::string_view decision_digest, std::string_view state_text,
    std::uint64_t revision, std::string_view diagnostic) {
    try {
        const auto e=nlohmann::json::parse(event_text), d=nlohmann::json::parse(decision_text);
        DurableTaskCoordinationCommand out; out.command_id=id;
        out.event.identity={e.at("tenant_id"),e.at("conversation_id")};
        out.event.task_id=e.at("task_id"); out.event.turn_id=e.at("turn_id");
        out.event.run_id=e.at("run_id"); out.event.harness_id=e.at("harness_id");
        out.event.task_revision=e.at("task_revision");
        out.event.source_event_id=e.at("source_event_id");
        out.decision.command=*([&]{ for(auto v:{CoordinationCommand::None,CoordinationCommand::ContinueExecution,CoordinationCommand::AwaitInput,CoordinationCommand::AwaitApproval,CoordinationCommand::AwaitExternal,CoordinationCommand::VerifyCompletion,CoordinationCommand::CloseVerified,CoordinationCommand::Fail,CoordinationCommand::Cancel,CoordinationCommand::ManualReview}) if(name(v)==d.at("command").get<std::string>()) return std::optional<CoordinationCommand>{v}; return std::optional<CoordinationCommand>{}; })();
        out.decision.task_state=*conversation::task_lifecycle_state(
            d.at("task_state").get<std::string>());
        out.decision.closure_state=d.at("closure_state"); out.decision.reason_code=d.at("reason_code");
        out.decision.terminal=d.at("terminal"); out.decision.digest=d.at("digest");
        if(out.decision.digest != decision_digest) return std::nullopt;
        out.state=command_state(state_text); out.journal_revision=revision;
        out.diagnostic=diagnostic; return out;
    } catch(...) { return std::nullopt; }
}
}

SQLiteTaskCoordinationJournal::SQLiteTaskCoordinationJournal(std::string path) {
    if(path.empty()) throw std::invalid_argument("task coordination journal path required");
    std::filesystem::path file(path); std::error_code ec;
    if(file.has_parent_path()) std::filesystem::create_directories(file.parent_path(),ec);
    sqlite3* opened=nullptr;
    if(ec || sqlite3_open_v2(path.c_str(),&opened,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,nullptr)!=SQLITE_OK) {
        std::string error=opened?sqlite3_errmsg(opened):ec.message(); if(opened)sqlite3_close(opened);
        throw std::runtime_error(error);
    }
    db_=opened; sqlite3_busy_timeout(opened,3000);
    internal::sqlite::exec(opened,"PRAGMA journal_mode=WAL");
    internal::sqlite::exec(opened,"PRAGMA synchronous=FULL");
    internal::sqlite::exec(opened,"CREATE TABLE IF NOT EXISTS task_coordination_commands("
        "command_id TEXT PRIMARY KEY,source_event_id TEXT NOT NULL UNIQUE,event_json TEXT NOT NULL,"
        "decision_json TEXT NOT NULL,decision_digest TEXT NOT NULL,state TEXT NOT NULL,"
        "revision INTEGER NOT NULL,diagnostic TEXT NOT NULL,updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)");
}
SQLiteTaskCoordinationJournal::~SQLiteTaskCoordinationJournal(){if(db_)sqlite3_close(internal::sqlite::database(db_));}

bool SQLiteTaskCoordinationJournal::submit(const DurableTaskCoordinationCommand& c,std::string* error){
    if(c.command_id.empty()||c.event.source_event_id.empty()||c.decision.digest.empty()){if(error)*error="task_coordination_command_invalid";return false;}
    std::lock_guard lock(mutex_); auto*db=internal::sqlite::database(db_);
    Statement q(db,"INSERT OR IGNORE INTO task_coordination_commands VALUES(?,?,?,?,?,'pending',1,'',CURRENT_TIMESTAMP)");
    internal::sqlite::bind_text(q.get(),1,c.command_id);internal::sqlite::bind_text(q.get(),2,c.event.source_event_id);
    internal::sqlite::bind_text(q.get(),3,encode_event(c.event).dump());internal::sqlite::bind_text(q.get(),4,encode_decision(c.decision).dump());
    internal::sqlite::bind_text(q.get(),5,c.decision.digest);
    if(internal::sqlite::step(q.get())!=SQLITE_DONE){if(error)*error=sqlite3_errmsg(db);return false;}
    Statement verify(db,"SELECT decision_digest FROM task_coordination_commands WHERE command_id=?");
    internal::sqlite::bind_text(verify.get(),1,c.command_id);
    const bool ok=internal::sqlite::step(verify.get())==SQLITE_ROW&&internal::sqlite::column_text(verify.get(),0)==c.decision.digest;
    if(!ok&&error)*error="task_coordination_idempotency_conflict"; return ok;
}
std::optional<DurableTaskCoordinationCommand> SQLiteTaskCoordinationJournal::load(std::string_view id){
    std::lock_guard lock(mutex_);auto*db=internal::sqlite::database(db_);Statement q(db,"SELECT event_json,decision_json,decision_digest,state,revision,diagnostic FROM task_coordination_commands WHERE command_id=?");internal::sqlite::bind_text(q.get(),1,id);if(internal::sqlite::step(q.get())!=SQLITE_ROW)return std::nullopt;return decode_command(id,internal::sqlite::column_text(q.get(),0),internal::sqlite::column_text(q.get(),1),internal::sqlite::column_text(q.get(),2),internal::sqlite::column_text(q.get(),3),internal::sqlite::column_uint64(q.get(),4),internal::sqlite::column_text(q.get(),5));
}
std::vector<DurableTaskCoordinationCommand> SQLiteTaskCoordinationJournal::pending(std::size_t limit){
    std::vector<std::string> ids;{std::lock_guard lock(mutex_);auto*db=internal::sqlite::database(db_);Statement q(db,"SELECT command_id FROM task_coordination_commands WHERE state='pending' ORDER BY updated_at,command_id LIMIT ?");internal::sqlite::bind_uint64(q.get(),1,limit);while(internal::sqlite::step(q.get())==SQLITE_ROW)ids.push_back(internal::sqlite::column_text(q.get(),0));}std::vector<DurableTaskCoordinationCommand> out;for(const auto&id:ids)if(auto c=load(id))out.push_back(*c);return out;
}
bool SQLiteTaskCoordinationJournal::transition(std::string_view id,std::uint64_t expected,TaskCoordinationCommandState state,std::string_view diagnostic,std::string*error){
    std::lock_guard lock(mutex_);auto*db=internal::sqlite::database(db_);Statement q(db,"UPDATE task_coordination_commands SET state=?,revision=revision+1,diagnostic=?,updated_at=CURRENT_TIMESTAMP WHERE command_id=? AND state='pending' AND revision=?");internal::sqlite::bind_text(q.get(),1,name(state));internal::sqlite::bind_text(q.get(),2,diagnostic);internal::sqlite::bind_text(q.get(),3,id);internal::sqlite::bind_uint64(q.get(),4,expected);if(internal::sqlite::step(q.get())!=SQLITE_DONE||internal::sqlite::changes(db)!=1){if(error)*error="task_coordination_journal_cas_conflict";return false;}return true;
}

bool DurableTaskStateCoordinator::publish(const CorrelatedStateEvent&e,const TaskCoordinationDecision&d,std::string*error){
    DurableTaskCoordinationCommand c;c.command_id=e.task_id+":"+e.source_event_id;
    if(journal_.load(c.command_id)) return reconcile(c.command_id,error);
    c.event=e;c.decision=d;if(!journal_.submit(c,error))return false;return reconcile(c.command_id,error);
}
bool DurableTaskStateCoordinator::reconcile(std::string_view id,std::string*error){
    auto c=journal_.load(id);if(!c){if(error)*error="task_coordination_command_not_found";return false;}if(c->state==TaskCoordinationCommandState::Applied)return true;if(c->state==TaskCoordinationCommandState::Conflict)return false;
    auto task=tasks_.load(c->event.identity,c->event.task_id);if(!task){journal_.transition(id,c->journal_revision,TaskCoordinationCommandState::Conflict,"task_not_found",nullptr);if(error)*error="task_not_found";return false;}
    if(task->state==c->decision.task_state&&task->closure_state==c->decision.closure_state)return journal_.transition(id,c->journal_revision,TaskCoordinationCommandState::Applied,"idempotent_state_match",error);
    if(task->revision!=c->event.task_revision){journal_.transition(id,c->journal_revision,TaskCoordinationCommandState::Conflict,"task_revision_conflict",nullptr);if(error)*error="task_revision_conflict";return false;}
    const auto changed=tasks_.transition(c->event.identity,c->event.task_id,task->revision,c->decision.task_state,c->decision.closure_state);if(!changed.ok){if(error)*error=changed.error;return false;}return journal_.transition(id,c->journal_revision,TaskCoordinationCommandState::Applied,"task_revision:"+std::to_string(changed.revision),error);
}
std::size_t DurableTaskStateCoordinator::reconcile_pending(std::size_t limit){std::size_t n=0;for(const auto&c:journal_.pending(limit)){std::string error;if(reconcile(c.command_id,&error))++n;}return n;}

bool TaskCoordinationPublisher::publish(CoordinationBoundary boundary,
                                        CorrelatedStateEvent event,
                                        std::string* error) {
    if(event.source_event_id.empty()) {
        if(error) *error = "task_coordination_source_event_required";
        return false;
    }
    event.source_event_id = std::string(name(boundary)) + ":" +
                            event.source_event_id;
    return durable_.publish(event, decisions_.observe(event), error);
}

}  // namespace agent_framework::recovery
