#include "agent/session/run_supervisor.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <stdexcept>

#include <sqlite3.h>
#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::session
{
    namespace
    {
        namespace sql = agent_framework::internal::sqlite;
        std::string stamp() { return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()); }
        std::optional<SupervisedRunState> state_from(std::string_view s)
        {
            for (std::size_t i = 0; i < 7; ++i)
            {
                auto v = static_cast<SupervisedRunState>(i);
                if (name(v) == s)
                    return v;
            }
            return {};
        }
        std::optional<SessionCommandKind> command_from(std::string_view s)
        {
            for (std::size_t i = 0; i < 9; ++i)
            {
                auto v = static_cast<SessionCommandKind>(i);
                if (name(v) == s)
                    return v;
            }
            return {};
        }
        SupervisedRun decode(sqlite3_stmt *q)
        {
            SupervisedRun v;
            int i = 0;
            v.request.tenant_id = sql::column_text(q, i++);
            v.request.organization_id = sql::column_text(q, i++);
            v.request.project_id = sql::column_text(q, i++);
            v.request.principal_id = sql::column_text(q, i++);
            v.request.provider_id = sql::column_text(q, i++);
            v.request.session_id = sql::column_text(q, i++);
            v.request.run_id = sql::column_text(q, i++);
            v.request.command_id = sql::column_text(q, i++);
            v.request.payload = nlohmann::json::parse(sql::column_text(q, i++));
            v.state = *state_from(sql::column_text(q, i++));
            v.revision = sql::column_uint64(q, i++);
            v.lease_epoch = sql::column_uint64(q, i++);
            v.lease_owner = sql::column_text(q, i++);
            v.lease_expires_at_ms = sql::column_uint64(q, i++);
            v.request.created_at = sql::column_text(q, i++);
            v.updated_at = sql::column_text(q, i++);
            v.command_cursor = sql::column_uint64(q, i++);
            return v;
        }
        constexpr const char *cols = "tenant,organization_id,project_id,principal_id,provider_id,session_id,run_id,command_id,payload_json,state,revision,lease_epoch,lease_owner,lease_expires_at_ms,created_at,updated_at,command_cursor";
        bool terminal(SupervisedRunState s) { return s == SupervisedRunState::Completed || s == SupervisedRunState::Failed || s == SupervisedRunState::Cancelled; }
    }
    std::string_view name(SupervisedRunState v)
    {
        static constexpr std::array<std::string_view, 7> n{"queued", "leased", "running", "awaiting_input", "completed", "failed", "cancelled"};
        auto i = static_cast<std::size_t>(v);
        return i < n.size() ? n[i] : "unknown";
    }
    std::string_view name(SessionCommandKind v)
    {
        static constexpr std::array<std::string_view, 9> n{
            "start", "steer", "queue", "comment", "fork", "cancel",
            "retry", "reconcile", "escalate"};
        auto i = static_cast<std::size_t>(v);
        return i < n.size() ? n[i] : "unknown";
    }
    SQLiteSessionRunSupervisor::SQLiteSessionRunSupervisor(std::string path, RunSupervisorQuota q) : path_(std::move(path)), quota_(q)
    {
        if (path_.empty())
            throw std::invalid_argument("run supervisor path required");
        std::error_code e;
        std::filesystem::path f(path_);
        if (f.has_parent_path())
            std::filesystem::create_directories(f.parent_path(), e);
        sqlite3 *db = nullptr;
        if (e || sqlite3_open_v2(path_.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
        {
            auto m = db ? sqlite3_errmsg(db) : e.message();
            if (db)
                sqlite3_close(db);
            throw std::runtime_error(m);
        }
        db_ = db;
        sqlite3_busy_timeout(db, 5000);
        sql::exec(db, "PRAGMA journal_mode=WAL");
        sql::exec(db, "PRAGMA synchronous=FULL");
        migrate();
    }
    SQLiteSessionRunSupervisor::~SQLiteSessionRunSupervisor()
    {
        if (db_)
            sqlite3_close(sql::database(db_));
    }
    void SQLiteSessionRunSupervisor::migrate()
    {
        auto *db = sql::database(db_);
        sql::exec(db, "CREATE TABLE IF NOT EXISTS supervised_session_runs(tenant TEXT NOT NULL,organization_id TEXT NOT NULL,project_id TEXT NOT NULL,principal_id TEXT NOT NULL,provider_id TEXT NOT NULL,session_id TEXT NOT NULL,run_id TEXT NOT NULL,command_id TEXT NOT NULL,payload_json TEXT NOT NULL,state TEXT NOT NULL,revision INTEGER NOT NULL,lease_epoch INTEGER NOT NULL,lease_owner TEXT NOT NULL,lease_expires_at_ms INTEGER NOT NULL,created_at TEXT NOT NULL,updated_at TEXT NOT NULL,command_cursor INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(tenant,run_id),UNIQUE(tenant,session_id,command_id))");
        if(!sql::table_has_column(db,"supervised_session_runs","command_cursor"))
            sql::exec(db,"ALTER TABLE supervised_session_runs ADD COLUMN command_cursor INTEGER NOT NULL DEFAULT 0");
        sql::exec(db, "CREATE INDEX IF NOT EXISTS supervised_runs_claim_idx ON supervised_session_runs(state,lease_expires_at_ms,created_at)");
        sql::exec(db, "CREATE TABLE IF NOT EXISTS supervised_session_commands(tenant TEXT NOT NULL,session_id TEXT NOT NULL,run_id TEXT NOT NULL,command_id TEXT NOT NULL,kind TEXT NOT NULL,payload_json TEXT NOT NULL,sequence INTEGER NOT NULL,created_at TEXT NOT NULL,PRIMARY KEY(tenant,session_id,command_id),UNIQUE(tenant,run_id,sequence))");
    }
    RunSupervisorResult SQLiteSessionRunSupervisor::enqueue(SessionRunRequest v)
    {
        if (v.tenant_id.empty() || v.organization_id.empty() || v.project_id.empty() || v.principal_id.empty() || v.provider_id.empty() || v.session_id.empty() || v.run_id.empty() || v.command_id.empty())
            return {false, 0, 0, "run_request_identity_required"};
        std::lock_guard l(mutex_);
        auto *db = sql::database(db_);
        try
        {
            sql::Transaction tx(db);
            sql::Statement old(db, "SELECT run_id,revision FROM supervised_session_runs WHERE tenant=? AND session_id=? AND command_id=?");
            sql::bind_text(old.get(), 1, v.tenant_id);
            sql::bind_text(old.get(), 2, v.session_id);
            sql::bind_text(old.get(), 3, v.command_id);
            if (sql::step(old.get()) == SQLITE_ROW)
            {
                bool same = sql::column_text(old.get(), 0) == v.run_id;
                return {same, sql::column_uint64(old.get(), 1), 0, same ? "" : "run_command_idempotency_conflict"};
            }
            if (v.created_at.empty())
                v.created_at = stamp();
            sql::Statement ins(db, "INSERT INTO supervised_session_runs(tenant,organization_id,project_id,principal_id,provider_id,session_id,run_id,command_id,payload_json,state,revision,lease_epoch,lease_owner,lease_expires_at_ms,created_at,updated_at,command_cursor) VALUES(?,?,?,?,?,?,?,?,?,'queued',1,0,'',0,?,?,0)");
            int i = 1;
            for (const auto *s : {&v.tenant_id, &v.organization_id, &v.project_id, &v.principal_id, &v.provider_id, &v.session_id, &v.run_id, &v.command_id})
                sql::bind_text(ins.get(), i++, *s);
            sql::bind_text(ins.get(), i++, v.payload.dump());
            sql::bind_text(ins.get(), i++, v.created_at);
            sql::bind_text(ins.get(), i++, v.created_at);
            if (sql::step(ins.get()) != SQLITE_DONE)
                throw std::runtime_error(sqlite3_errmsg(db));
            SessionRunCommand c{v.tenant_id, v.session_id, v.run_id, v.command_id, SessionCommandKind::Start, v.payload, 1, v.created_at};
            sql::Statement ci(db, "INSERT INTO supervised_session_commands VALUES(?,?,?,?,?,?,?,?)");
            sql::bind_text(ci.get(), 1, c.tenant_id);
            sql::bind_text(ci.get(), 2, c.session_id);
            sql::bind_text(ci.get(), 3, c.run_id);
            sql::bind_text(ci.get(), 4, c.command_id);
            sql::bind_text(ci.get(), 5, name(c.kind));
            sql::bind_text(ci.get(), 6, c.payload.dump());
            sql::bind_uint64(ci.get(), 7, 1);
            sql::bind_text(ci.get(), 8, c.created_at);
            if (sql::step(ci.get()) != SQLITE_DONE)
                throw std::runtime_error(sqlite3_errmsg(db));
            tx.commit();
            return {true, 1, 0, {}};
        }
        catch (const std::exception &e)
        {
            return {false, 0, 0, e.what()};
        }
    }
    RunSupervisorResult SQLiteSessionRunSupervisor::enqueue_command(SessionRunCommand c)
    {
        if (c.tenant_id.empty() || c.session_id.empty() || c.run_id.empty() || c.command_id.empty() || c.kind == SessionCommandKind::Start)
            return {false, 0, 0, "session_command_contract_invalid"};
        std::lock_guard l(mutex_);
        auto *db = sql::database(db_);
        try
        {
            sql::Transaction tx(db);
            sql::Statement old(db, "SELECT run_id,kind,payload_json,sequence FROM supervised_session_commands WHERE tenant=? AND session_id=? AND command_id=?");
            sql::bind_text(old.get(), 1, c.tenant_id);
            sql::bind_text(old.get(), 2, c.session_id);
            sql::bind_text(old.get(), 3, c.command_id);
            if (sql::step(old.get()) == SQLITE_ROW)
            {
                bool same = sql::column_text(old.get(), 0) == c.run_id &&
                            sql::column_text(old.get(), 1) == name(c.kind) &&
                            sql::column_text(old.get(), 2) == c.payload.dump();
                return {same, sql::column_uint64(old.get(), 3), 0,
                        same ? "" : "run_command_idempotency_conflict"};
            }
            sql::Statement parent(db, "SELECT revision,state FROM supervised_session_runs WHERE tenant=? AND session_id=? AND run_id=?");
            sql::bind_text(parent.get(), 1, c.tenant_id);
            sql::bind_text(parent.get(), 2, c.session_id);
            sql::bind_text(parent.get(), 3, c.run_id);
            if (sql::step(parent.get()) != SQLITE_ROW)
                return {false, 0, 0, "run_not_found_or_session_mismatch"};
            const auto current_revision=sql::column_uint64(parent.get(),0);
            const auto current_state=state_from(sql::column_text(parent.get(),1));
            if(!current_state)return {false,current_revision,0,"stored_run_state_invalid"};
            const bool command_allowed=c.kind==SessionCommandKind::Comment||
                (c.kind==SessionCommandKind::Retry&&*current_state==SupervisedRunState::Failed)||
                ((c.kind==SessionCommandKind::Reconcile||c.kind==SessionCommandKind::Escalate)&&
                    (*current_state==SupervisedRunState::Failed||
                     *current_state==SupervisedRunState::AwaitingInput))||
                (!terminal(*current_state)&&c.kind!=SessionCommandKind::Retry&&
                    c.kind!=SessionCommandKind::Reconcile&&c.kind!=SessionCommandKind::Escalate);
            if(!command_allowed)
                return {false,current_revision,0,"run_command_invalid_for_state"};
            if(!c.expected_run_revision)
                return {false,current_revision,0,"expected_run_revision_required"};
            if(*c.expected_run_revision!=current_revision)
                return {false,current_revision,0,"run_revision_conflict"};
            sql::Statement tail(db, "SELECT COALESCE(MAX(sequence),0) FROM supervised_session_commands WHERE tenant=? AND run_id=?");
            sql::bind_text(tail.get(), 1, c.tenant_id);
            sql::bind_text(tail.get(), 2, c.run_id);
            sql::step(tail.get());
            c.sequence = sql::column_uint64(tail.get(), 0) + 1;
            if (c.created_at.empty())
                c.created_at = stamp();
            sql::Statement ins(db, "INSERT INTO supervised_session_commands VALUES(?,?,?,?,?,?,?,?)");
            sql::bind_text(ins.get(), 1, c.tenant_id);
            sql::bind_text(ins.get(), 2, c.session_id);
            sql::bind_text(ins.get(), 3, c.run_id);
            sql::bind_text(ins.get(), 4, c.command_id);
            sql::bind_text(ins.get(), 5, name(c.kind));
            sql::bind_text(ins.get(), 6, c.payload.dump());
            sql::bind_uint64(ins.get(), 7, c.sequence);
            sql::bind_text(ins.get(), 8, c.created_at);
            if (sql::step(ins.get()) != SQLITE_DONE)
                throw std::runtime_error(sqlite3_errmsg(db));
            // Awaiting-input is a durable parked state, not an expiring worker
            // lease. A material command explicitly wakes it and invalidates the
            // old fencing epoch; comments remain non-executing annotations.
            if(c.kind!=SessionCommandKind::Comment) {
                const char* wake_states=(c.kind==SessionCommandKind::Retry)?"state='failed'":
                    (c.kind==SessionCommandKind::Reconcile||c.kind==SessionCommandKind::Escalate)?
                        "(state='failed' OR state='awaiting_input')":"state='awaiting_input'";
                const auto wake_sql=std::string("UPDATE supervised_session_runs SET state='queued',")+
                    "revision=revision+1,lease_owner='',lease_expires_at_ms=0,updated_at=? "
                    "WHERE tenant=? AND run_id=? AND "+wake_states;
                sql::Statement wake(db,wake_sql.c_str());
                sql::bind_text(wake.get(),1,c.created_at);sql::bind_text(wake.get(),2,c.tenant_id);
                sql::bind_text(wake.get(),3,c.run_id);
                if(sql::step(wake.get())!=SQLITE_DONE)
                    throw std::runtime_error(sqlite3_errmsg(db));
            }
            tx.commit();
            return {true, c.sequence, 0, {}};
        }
        catch (const std::exception &e)
        {
            return {false, 0, 0, e.what()};
        }
    }

    std::optional<SupervisedRun> SQLiteSessionRunSupervisor::claim_next(
        std::string_view worker, std::uint64_t now, std::uint64_t lease_ms, std::string *error)
    {
        if (worker.empty() || lease_ms == 0)
        {
            if (error)
                *error = "lease_contract_invalid";
            return {};
        }
        std::lock_guard l(mutex_);
        auto *db = sql::database(db_);
        try
        {
            sql::Transaction tx(db);
            const auto query = std::string("SELECT ") + cols + " FROM supervised_session_runs WHERE state='queued' OR ((state='leased' OR state='running') AND lease_expires_at_ms<?) ORDER BY created_at,run_id LIMIT 128";
            sql::Statement candidates(db, query.c_str());
            sql::bind_uint64(candidates.get(), 1, now);
            auto active_count = [&](const char *column, const SupervisedRun &candidate, std::string_view value)
            {auto statement=std::string("SELECT COUNT(*) FROM supervised_session_runs WHERE tenant=? AND ")+column+"=? AND (state='leased' OR state='running') AND lease_expires_at_ms>=?";sql::Statement q(db,statement.c_str());sql::bind_text(q.get(),1,candidate.request.tenant_id);sql::bind_text(q.get(),2,value);sql::bind_uint64(q.get(),3,now);sql::step(q.get());return static_cast<std::size_t>(sql::column_uint64(q.get(),0)); };
            while (sql::step(candidates.get()) == SQLITE_ROW)
            {
                auto run = decode(candidates.get());
                if (active_count("session_id", run, run.request.session_id) > 0)
                    continue;
                if (active_count("organization_id", run, run.request.organization_id) >= quota_.organization_active || active_count("project_id", run, run.request.project_id) >= quota_.project_active || active_count("principal_id", run, run.request.principal_id) >= quota_.principal_active || active_count("provider_id", run, run.request.provider_id) >= quota_.provider_active)
                    continue;
                sql::Statement update(db, "UPDATE supervised_session_runs SET state='leased',revision=revision+1,lease_epoch=lease_epoch+1,lease_owner=?,lease_expires_at_ms=?,updated_at=? WHERE tenant=? AND run_id=? AND revision=? AND (state='queued' OR ((state='leased' OR state='running' OR state='awaiting_input') AND lease_expires_at_ms<?))");
                sql::bind_text(update.get(), 1, worker);
                sql::bind_uint64(update.get(), 2, now + lease_ms);
                sql::bind_text(update.get(), 3, std::to_string(now));
                sql::bind_text(update.get(), 4, run.request.tenant_id);
                sql::bind_text(update.get(), 5, run.request.run_id);
                sql::bind_uint64(update.get(), 6, run.revision);
                sql::bind_uint64(update.get(), 7, now);
                if (sql::step(update.get()) != SQLITE_DONE || sql::changes(db) != 1)
                    continue;
                run.state = SupervisedRunState::Leased;
                ++run.revision;
                ++run.lease_epoch;
                run.lease_owner = std::string(worker);
                run.lease_expires_at_ms = now + lease_ms;
                run.updated_at = std::to_string(now);
                tx.commit();
                return run;
            }
            tx.commit();
            if (error)
                *error = "no_eligible_run";
            return {};
        }
        catch (const std::exception &e)
        {
            if (error)
                *error = e.what();
            return {};
        }
    }

    RunSupervisorResult SQLiteSessionRunSupervisor::mark_running(std::string_view tenant, std::string_view run, std::string_view worker, std::uint64_t epoch, std::uint64_t expected)
    {
        std::lock_guard l(mutex_);
        auto *db = sql::database(db_);
        sql::Statement q(db, "UPDATE supervised_session_runs SET state='running',revision=revision+1,updated_at=? WHERE tenant=? AND run_id=? AND state='leased' AND lease_owner=? AND lease_epoch=? AND revision=?");
        sql::bind_text(q.get(), 1, stamp());
        sql::bind_text(q.get(), 2, tenant);
        sql::bind_text(q.get(), 3, run);
        sql::bind_text(q.get(), 4, worker);
        sql::bind_uint64(q.get(), 5, epoch);
        sql::bind_uint64(q.get(), 6, expected);
        if (sql::step(q.get()) != SQLITE_DONE)
            return {false, expected, epoch, sqlite3_errmsg(db)};
        return sql::changes(db) == 1 ? RunSupervisorResult{true, expected + 1, epoch, {}} : RunSupervisorResult{false, expected, epoch, "lease_fence_or_revision_conflict"};
    }
    RunSupervisorResult SQLiteSessionRunSupervisor::renew(std::string_view tenant, std::string_view run, std::string_view worker, std::uint64_t epoch, std::uint64_t expected, std::uint64_t expires)
    {
        std::lock_guard l(mutex_);
        auto *db = sql::database(db_);
        sql::Statement q(db, "UPDATE supervised_session_runs SET revision=revision+1,lease_expires_at_ms=?,updated_at=? WHERE tenant=? AND run_id=? AND (state='leased' OR state='running' OR state='awaiting_input') AND lease_owner=? AND lease_epoch=? AND revision=?");
        sql::bind_uint64(q.get(), 1, expires);
        sql::bind_text(q.get(), 2, stamp());
        sql::bind_text(q.get(), 3, tenant);
        sql::bind_text(q.get(), 4, run);
        sql::bind_text(q.get(), 5, worker);
        sql::bind_uint64(q.get(), 6, epoch);
        sql::bind_uint64(q.get(), 7, expected);
        if (sql::step(q.get()) != SQLITE_DONE)
            return {false, expected, epoch, sqlite3_errmsg(db)};
        return sql::changes(db) == 1 ? RunSupervisorResult{true, expected + 1, epoch, {}} : RunSupervisorResult{false, expected, epoch, "lease_fence_or_revision_conflict"};
    }
    RunSupervisorResult SQLiteSessionRunSupervisor::await_input(std::string_view tenant, std::string_view run, std::string_view worker, std::uint64_t epoch, std::uint64_t expected, std::uint64_t expires,std::uint64_t cursor)
    {
        std::lock_guard l(mutex_);
        auto *db = sql::database(db_);
        sql::Statement q(db, "UPDATE supervised_session_runs SET state='awaiting_input',revision=revision+1,lease_expires_at_ms=?,updated_at=?,command_cursor=MAX(command_cursor,?) WHERE tenant=? AND run_id=? AND state='running' AND lease_owner=? AND lease_epoch=? AND revision=?");
        sql::bind_uint64(q.get(), 1, expires);
        sql::bind_text(q.get(), 2, stamp());
        sql::bind_uint64(q.get(), 3, cursor);
        sql::bind_text(q.get(), 4, tenant);
        sql::bind_text(q.get(), 5, run);
        sql::bind_text(q.get(), 6, worker);
        sql::bind_uint64(q.get(), 7, epoch);
        sql::bind_uint64(q.get(), 8, expected);
        if (sql::step(q.get()) != SQLITE_DONE)
            return {false, expected, epoch, sqlite3_errmsg(db)};
        return sql::changes(db) == 1 ? RunSupervisorResult{true, expected + 1, epoch, {}} : RunSupervisorResult{false, expected, epoch, "lease_fence_or_revision_conflict"};
    }
    RunSupervisorResult SQLiteSessionRunSupervisor::finish(std::string_view tenant, std::string_view run, std::string_view worker, std::uint64_t epoch, std::uint64_t expected, SupervisedRunState state,std::uint64_t cursor)
    {
        if (!terminal(state))
            return {false, expected, epoch, "terminal_state_required"};
        std::lock_guard l(mutex_);
        auto *db = sql::database(db_);
        sql::Statement q(db, "UPDATE supervised_session_runs SET state=?,revision=revision+1,lease_expires_at_ms=0,updated_at=?,command_cursor=MAX(command_cursor,?) WHERE tenant=? AND run_id=? AND (state='leased' OR state='running' OR state='awaiting_input') AND lease_owner=? AND lease_epoch=? AND revision=?");
        sql::bind_text(q.get(), 1, name(state));
        sql::bind_text(q.get(), 2, stamp());
        sql::bind_uint64(q.get(), 3, cursor);
        sql::bind_text(q.get(), 4, tenant);
        sql::bind_text(q.get(), 5, run);
        sql::bind_text(q.get(), 6, worker);
        sql::bind_uint64(q.get(), 7, epoch);
        sql::bind_uint64(q.get(), 8, expected);
        if (sql::step(q.get()) != SQLITE_DONE)
            return {false, expected, epoch, sqlite3_errmsg(db)};
        return sql::changes(db) == 1 ? RunSupervisorResult{true, expected + 1, epoch, {}} : RunSupervisorResult{false, expected, epoch, "lease_fence_or_revision_conflict"};
    }
    std::optional<SupervisedRun> SQLiteSessionRunSupervisor::load(std::string_view tenant, std::string_view run)
    {
        std::lock_guard l(mutex_);
        auto query = std::string("SELECT ") + cols + " FROM supervised_session_runs WHERE tenant=? AND run_id=?";
        sql::Statement q(sql::database(db_), query.c_str());
        sql::bind_text(q.get(), 1, tenant);
        sql::bind_text(q.get(), 2, run);
        if (sql::step(q.get()) != SQLITE_ROW)
            return {};
        return decode(q.get());
    }
    std::vector<SessionRunCommand> SQLiteSessionRunSupervisor::commands(std::string_view tenant, std::string_view run,std::uint64_t after)
    {
        std::lock_guard l(mutex_);
        sql::Statement q(sql::database(db_), "SELECT session_id,command_id,kind,payload_json,sequence,created_at FROM supervised_session_commands WHERE tenant=? AND run_id=? AND sequence>? ORDER BY sequence");
        sql::bind_text(q.get(), 1, tenant);
        sql::bind_text(q.get(), 2, run);
        sql::bind_uint64(q.get(), 3, after);
        std::vector<SessionRunCommand> out;
        while (sql::step(q.get()) == SQLITE_ROW)
        {
            SessionRunCommand c;
            c.tenant_id = std::string(tenant);
            c.session_id = sql::column_text(q.get(), 0);
            c.run_id = std::string(run);
            c.command_id = sql::column_text(q.get(), 1);
            c.kind = *command_from(sql::column_text(q.get(), 2));
            c.payload = nlohmann::json::parse(sql::column_text(q.get(), 3));
            c.sequence = sql::column_uint64(q.get(), 4);
            c.created_at = sql::column_text(q.get(), 5);
            out.push_back(std::move(c));
        }
        return out;
    }
} // namespace agent_framework::session
