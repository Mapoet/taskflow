#include "agent/run/store.hpp"

#include <filesystem>
#include <stdexcept>
#include <utility>

#include <sqlite3.h>

#include "agent/contracts/contract.hpp"
#include "agent/internal/sqlite_utils.hpp"
#include "agent/run/state_machine.hpp"

namespace agent_framework::run {
namespace {

using json = nlohmann::json;
namespace sqlite = internal::sqlite;

StoreResult sqlite_failure(sqlite3* db, int rc) {
    return {rc == SQLITE_BUSY || rc == SQLITE_LOCKED ? StoreStatus::Busy : StoreStatus::Error,
            0, sqlite3_errmsg(db)};
}

std::optional<RunCheckpoint> checkpoint_from_text(const std::string& text) {
    try { return decode_run_checkpoint(json::parse(text)); }
    catch(...) { return std::nullopt; }
}

bool valid_checkpoint(const RunCheckpoint& value, std::string* error) {
    std::vector<contracts::ContractIssue> issues;
    if(!contracts::validate_metadata(value.metadata, &issues) ||
       value.metadata.identity.run_id.empty()) {
        if(error) *error = issues.empty() ? "run_id is required" : issues.front().message;
        return false;
    }
    return true;
}

}  // namespace

SQLiteRunStore::SQLiteRunStore(std::string path, SQLiteRunStoreOptions options)
    : path_(std::move(path)), options_(options) {
    if(path_.empty()) throw std::invalid_argument("run store path must not be empty");
    const std::filesystem::path file(path_);
    std::error_code error;
    if(file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), error);
    if(error) throw std::runtime_error("unable to create run store directory: " + error.message());
    sqlite3* db = nullptr;
    const int rc = sqlite3_open_v2(path_.c_str(), &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if(rc != SQLITE_OK) {
        const std::string message = db ? sqlite3_errmsg(db) : "sqlite open failed";
        if(db) sqlite3_close(db);
        throw std::runtime_error(message);
    }
    db_ = db;
    sqlite3_busy_timeout(db, options_.busy_timeout_ms);
    sqlite::exec(db, "PRAGMA journal_mode=WAL");
    sqlite::exec(db, "PRAGMA synchronous=FULL");
    sqlite::exec(db, "PRAGMA foreign_keys=ON");
    migrate();
#if !defined(_WIN32)
    if(options_.require_private_permissions) {
        std::filesystem::permissions(file,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace, error);
        if(error) throw std::runtime_error("unable to set private run store permissions: " + error.message());
    }
#endif
}

SQLiteRunStore::~SQLiteRunStore() {
    if(db_) sqlite3_close(sqlite::database(db_));
}

void SQLiteRunStore::migrate() {
    auto* db = sqlite::database(db_);
    sqlite::Transaction transaction(db);
    sqlite::exec(db, "CREATE TABLE IF NOT EXISTS run_schema_version("
             "version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL)");
    int version = 0;
    {
        sqlite::Statement query(db, "SELECT COALESCE(MAX(version),0) FROM run_schema_version");
        if(sqlite3_step(query.get()) == SQLITE_ROW) version = sqlite3_column_int(query.get(), 0);
    }
    if(version > 1) throw std::runtime_error("run store schema is newer than this binary");
    if(version == 0) {
        sqlite::exec(db, "CREATE TABLE runs("
                 "run_id TEXT PRIMARY KEY, tenant_id TEXT NOT NULL, task_id TEXT NOT NULL,"
                 "revision INTEGER NOT NULL, state TEXT NOT NULL, checkpoint_json TEXT NOT NULL,"
                 "graph_revision TEXT NOT NULL, plan_digest TEXT NOT NULL,"
                 "updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')))" );
        sqlite::exec(db, "CREATE INDEX runs_recovery_idx ON runs(state,updated_at)");
        sqlite::exec(db, "CREATE TABLE run_events("
                 "run_id TEXT NOT NULL, sequence INTEGER NOT NULL, event_type TEXT NOT NULL,"
                 "payload_json TEXT NOT NULL, payload_digest TEXT NOT NULL,"
                 "created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),"
                 "PRIMARY KEY(run_id,sequence), FOREIGN KEY(run_id) REFERENCES runs(run_id))");
        sqlite::exec(db, "CREATE TABLE run_interruptions("
                 "interruption_id TEXT PRIMARY KEY, run_id TEXT NOT NULL, token_digest TEXT NOT NULL,"
                 "document_json TEXT NOT NULL, consumed INTEGER NOT NULL DEFAULT 0,"
                 "expires_at TEXT NOT NULL, FOREIGN KEY(run_id) REFERENCES runs(run_id))");
        sqlite::exec(db, "CREATE TABLE graph_definitions("
                 "template_id TEXT NOT NULL, revision TEXT NOT NULL, definition_digest TEXT NOT NULL,"
                 "compatibility_class TEXT NOT NULL, PRIMARY KEY(template_id,revision))");
        sqlite::exec(db, "CREATE TABLE durable_timers("
                 "timer_id TEXT PRIMARY KEY, run_id TEXT NOT NULL, due_unix_ms INTEGER NOT NULL,"
                 "payload_json TEXT NOT NULL, owner TEXT NOT NULL DEFAULT '',"
                 "lease_until_unix_ms INTEGER NOT NULL DEFAULT 0, completed INTEGER NOT NULL DEFAULT 0,"
                 "FOREIGN KEY(run_id) REFERENCES runs(run_id))");
        sqlite::exec(db, "CREATE INDEX durable_timers_due_idx ON durable_timers(completed,due_unix_ms,lease_until_unix_ms)");
        sqlite::exec(db, "INSERT INTO run_schema_version(version,applied_at) "
                 "VALUES(1,strftime('%Y-%m-%dT%H:%M:%fZ','now'))");
    }
    transaction.commit();
}

StoreResult SQLiteRunStore::create(const RunCheckpoint& initial) {
    std::lock_guard lock(mutex_);
    std::string validation_error;
    if(!valid_checkpoint(initial, &validation_error))
        return {StoreStatus::Invalid, 0, validation_error};
    auto* db = sqlite::database(db_);
    const auto document = encode(initial).dump();
    sqlite::Statement statement(db, "INSERT INTO runs(run_id,tenant_id,task_id,revision,state,checkpoint_json,"
                            "graph_revision,plan_digest) VALUES(?,?,?,?,?,?,?,?)");
    sqlite::bind_text(statement.get(), 1, initial.metadata.identity.run_id);
    sqlite::bind_text(statement.get(), 2, initial.metadata.identity.tenant_id);
    sqlite::bind_text(statement.get(), 3, initial.metadata.identity.task_id);
    sqlite3_bind_int64(statement.get(), 4, 1);
    sqlite::bind_text(statement.get(), 5, run_state_name(initial.state));
    sqlite::bind_text(statement.get(), 6, document);
    sqlite::bind_text(statement.get(), 7, initial.graph_revision);
    sqlite::bind_text(statement.get(), 8, initial.plan_digest);
    const int rc = sqlite3_step(statement.get());
    if(rc == SQLITE_CONSTRAINT) return {StoreStatus::AlreadyExists, 0, "run already exists"};
    if(rc != SQLITE_DONE) return sqlite_failure(db, rc);
    return {StoreStatus::Committed, 1, {}};
}

std::optional<RunRecord> SQLiteRunStore::load(std::string_view run_id) {
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    sqlite::Statement statement(db, "SELECT revision,checkpoint_json,updated_at FROM runs WHERE run_id=?");
    sqlite::bind_text(statement.get(), 1, run_id);
    if(sqlite3_step(statement.get()) != SQLITE_ROW) return std::nullopt;
    auto checkpoint = checkpoint_from_text(sqlite::column_text(statement.get(), 1));
    if(!checkpoint) throw std::runtime_error("stored run checkpoint is corrupt");
    return RunRecord{std::move(*checkpoint),
                     static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 0)),
                     sqlite::column_text(statement.get(), 2)};
}

StoreResult SQLiteRunStore::checkpoint(const RunCheckpoint& next, std::uint64_t expected_revision) {
    std::lock_guard lock(mutex_);
    std::string validation_error;
    if(!valid_checkpoint(next, &validation_error))
        return {StoreStatus::Invalid, 0, validation_error};
    auto* db = sqlite::database(db_);
    try {
        sqlite::Transaction transaction(db);
        RunState prior_state;
        std::uint64_t actual_revision = 0;
        {
            sqlite::Statement query(db, "SELECT revision,checkpoint_json FROM runs WHERE run_id=?");
            sqlite::bind_text(query.get(), 1, next.metadata.identity.run_id);
            if(sqlite3_step(query.get()) != SQLITE_ROW)
                return {StoreStatus::NotFound, 0, "run not found"};
            actual_revision = static_cast<std::uint64_t>(sqlite3_column_int64(query.get(), 0));
            auto prior = checkpoint_from_text(sqlite::column_text(query.get(), 1));
            if(!prior) return {StoreStatus::Error, actual_revision, "stored checkpoint is corrupt"};
            prior_state = prior->state;
        }
        if(actual_revision != expected_revision)
            return {StoreStatus::RevisionConflict, actual_revision, "run revision conflict"};
        if(!can_transition(prior_state, next.state))
            return {StoreStatus::Invalid, actual_revision, "illegal run state transition"};
        sqlite::Statement update(db, "UPDATE runs SET revision=revision+1,state=?,checkpoint_json=?,"
                             "graph_revision=?,plan_digest=?,updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') "
                             "WHERE run_id=? AND revision=?");
        sqlite::bind_text(update.get(), 1, run_state_name(next.state));
        sqlite::bind_text(update.get(), 2, encode(next).dump());
        sqlite::bind_text(update.get(), 3, next.graph_revision);
        sqlite::bind_text(update.get(), 4, next.plan_digest);
        sqlite::bind_text(update.get(), 5, next.metadata.identity.run_id);
        sqlite3_bind_int64(update.get(), 6, static_cast<sqlite3_int64>(expected_revision));
        const int rc = sqlite3_step(update.get());
        if(rc != SQLITE_DONE) return sqlite_failure(db, rc);
        if(sqlite3_changes(db) != 1)
            return {StoreStatus::RevisionConflict, actual_revision, "run revision changed concurrently"};
        transaction.commit();
        return {StoreStatus::Committed, expected_revision + 1, {}};
    } catch(const std::exception& error) {
        return {StoreStatus::Error, 0, error.what()};
    }
}

std::vector<RunRecord> SQLiteRunStore::list_recoverable(std::size_t limit) {
    std::lock_guard lock(mutex_);
    std::vector<RunRecord> result;
    auto* db = sqlite::database(db_);
    sqlite::Statement statement(db, "SELECT revision,checkpoint_json,updated_at FROM runs "
                            "WHERE state NOT IN ('completed','partial','rejected','failed','cancelled') "
                            "ORDER BY updated_at LIMIT ?");
    sqlite3_bind_int64(statement.get(), 1, static_cast<sqlite3_int64>(limit));
    while(sqlite3_step(statement.get()) == SQLITE_ROW) {
        auto checkpoint = checkpoint_from_text(sqlite::column_text(statement.get(), 1));
        if(!checkpoint) throw std::runtime_error("stored run checkpoint is corrupt");
        result.push_back({std::move(*checkpoint),
            static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 0)),
            sqlite::column_text(statement.get(), 2)});
    }
    return result;
}

StoreResult SQLiteRunStore::append_event(const RunEvent& event) {
    std::lock_guard lock(mutex_);
    if(event.run_id.empty() || event.event_type.empty())
        return {StoreStatus::Invalid, 0, "run_id and event_type are required"};
    auto* db = sqlite::database(db_);
    try {
        sqlite::Transaction transaction(db);
        std::uint64_t sequence = event.sequence;
        if(sequence == 0) {
            sqlite::Statement query(db, "SELECT COALESCE(MAX(sequence),0)+1 FROM run_events WHERE run_id=?");
            sqlite::bind_text(query.get(), 1, event.run_id);
            if(sqlite3_step(query.get()) == SQLITE_ROW)
                sequence = static_cast<std::uint64_t>(sqlite3_column_int64(query.get(), 0));
        }
        const auto digest = event.payload_digest.empty()
            ? contracts::embedded_digest(event.payload).value_or("") : event.payload_digest;
        sqlite::Statement insert(db, "INSERT INTO run_events(run_id,sequence,event_type,payload_json,payload_digest)"
                             " VALUES(?,?,?,?,?)");
        sqlite::bind_text(insert.get(), 1, event.run_id);
        sqlite3_bind_int64(insert.get(), 2, static_cast<sqlite3_int64>(sequence));
        sqlite::bind_text(insert.get(), 3, event.event_type);
        sqlite::bind_text(insert.get(), 4, event.payload.dump());
        sqlite::bind_text(insert.get(), 5, digest);
        const int rc = sqlite3_step(insert.get());
        if(rc == SQLITE_CONSTRAINT)
            return {StoreStatus::RevisionConflict, sequence, "event sequence already exists"};
        if(rc != SQLITE_DONE) return sqlite_failure(db, rc);
        transaction.commit();
        return {StoreStatus::Committed, sequence, {}};
    } catch(const std::exception& error) { return {StoreStatus::Error, 0, error.what()}; }
}

std::vector<RunEvent> SQLiteRunStore::events(std::string_view run_id,
                                             std::uint64_t after_sequence) {
    std::lock_guard lock(mutex_);
    std::vector<RunEvent> result;
    auto* db = sqlite::database(db_);
    sqlite::Statement statement(db, "SELECT sequence,event_type,payload_json,payload_digest,created_at "
                            "FROM run_events WHERE run_id=? AND sequence>? ORDER BY sequence");
    sqlite::bind_text(statement.get(), 1, run_id);
    sqlite3_bind_int64(statement.get(), 2, static_cast<sqlite3_int64>(after_sequence));
    while(sqlite3_step(statement.get()) == SQLITE_ROW) {
        result.push_back({std::string(run_id),
            static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 0)),
            sqlite::column_text(statement.get(), 1), json::parse(sqlite::column_text(statement.get(), 2)),
            sqlite::column_text(statement.get(), 3), sqlite::column_text(statement.get(), 4)});
    }
    return result;
}

StoreResult SQLiteRunStore::put_interruption(const Interruption& interruption) {
    std::lock_guard lock(mutex_);
    const auto& run_id = interruption.metadata.identity.run_id;
    if(run_id.empty() || interruption.interruption_id.empty() || interruption.resume_token_digest.empty())
        return {StoreStatus::Invalid, 0, "interruption, run and token identities are required"};
    auto* db = sqlite::database(db_);
    sqlite::Statement statement(db, "INSERT INTO run_interruptions(interruption_id,run_id,token_digest,"
                            "document_json,expires_at) VALUES(?,?,?,?,?)");
    sqlite::bind_text(statement.get(), 1, interruption.interruption_id);
    sqlite::bind_text(statement.get(), 2, run_id);
    sqlite::bind_text(statement.get(), 3, interruption.resume_token_digest);
    sqlite::bind_text(statement.get(), 4, encode(interruption).dump());
    sqlite::bind_text(statement.get(), 5, interruption.expires_at);
    const int rc = sqlite3_step(statement.get());
    if(rc == SQLITE_CONSTRAINT) return {StoreStatus::AlreadyExists, 0, "interruption already exists"};
    if(rc != SQLITE_DONE) return sqlite_failure(db, rc);
    return {StoreStatus::Committed, 1, {}};
}

std::optional<Interruption> SQLiteRunStore::load_interruption(std::string_view interruption_id) {
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    sqlite::Statement statement(db, "SELECT document_json FROM run_interruptions WHERE interruption_id=?");
    sqlite::bind_text(statement.get(), 1, interruption_id);
    if(sqlite3_step(statement.get()) != SQLITE_ROW) return std::nullopt;
    auto value = decode_interruption(json::parse(sqlite::column_text(statement.get(), 0)));
    if(!value) throw std::runtime_error("stored interruption is corrupt");
    return value;
}

StoreResult SQLiteRunStore::consume_resume_token(std::string_view interruption_id,
    std::string_view run_id, std::string_view token_digest) {
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    sqlite::Statement statement(db, "UPDATE run_interruptions SET consumed=1 WHERE interruption_id=? "
                            "AND run_id=? AND token_digest=? AND consumed=0 "
                            "AND (expires_at='' OR expires_at>strftime('%Y-%m-%dT%H:%M:%fZ','now'))");
    sqlite::bind_text(statement.get(), 1, interruption_id);
    sqlite::bind_text(statement.get(), 2, run_id);
    sqlite::bind_text(statement.get(), 3, token_digest);
    const int rc = sqlite3_step(statement.get());
    if(rc != SQLITE_DONE) return sqlite_failure(db, rc);
    if(sqlite3_changes(db) != 1)
        return {StoreStatus::Invalid, 0, "resume token is invalid, expired, or already consumed"};
    return {StoreStatus::Committed, 1, {}};
}

StoreResult SQLiteRunStore::register_graph(const GraphDefinitionRef& graph) {
    std::lock_guard lock(mutex_);
    if(graph.template_id.empty() || graph.revision.empty() || graph.definition_digest.empty())
        return {StoreStatus::Invalid, 0, "graph identity and digest are required"};
    auto* db = sqlite::database(db_);
    sqlite::Statement query(db, "SELECT definition_digest,compatibility_class FROM graph_definitions "
                        "WHERE template_id=? AND revision=?");
    sqlite::bind_text(query.get(), 1, graph.template_id);
    sqlite::bind_text(query.get(), 2, graph.revision);
    if(sqlite3_step(query.get()) == SQLITE_ROW) {
        const bool same = sqlite::column_text(query.get(), 0) == graph.definition_digest &&
                          sqlite::column_text(query.get(), 1) == graph.compatibility_class;
        return {same ? StoreStatus::Committed : StoreStatus::RevisionConflict, 0,
                same ? std::string() : "graph revision digest mismatch"};
    }
    sqlite::Statement insert(db, "INSERT INTO graph_definitions(template_id,revision,definition_digest,"
                         "compatibility_class) VALUES(?,?,?,?)");
    sqlite::bind_text(insert.get(), 1, graph.template_id); sqlite::bind_text(insert.get(), 2, graph.revision);
    sqlite::bind_text(insert.get(), 3, graph.definition_digest); sqlite::bind_text(insert.get(), 4, graph.compatibility_class);
    const int rc = sqlite3_step(insert.get());
    return rc == SQLITE_DONE ? StoreResult{StoreStatus::Committed, 1, {}} : sqlite_failure(db, rc);
}

std::optional<GraphDefinitionRef> SQLiteRunStore::load_graph(std::string_view template_id,
                                                             std::string_view revision) {
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    sqlite::Statement query(db, "SELECT definition_digest,compatibility_class FROM graph_definitions "
                        "WHERE template_id=? AND revision=?");
    sqlite::bind_text(query.get(), 1, template_id); sqlite::bind_text(query.get(), 2, revision);
    if(sqlite3_step(query.get()) != SQLITE_ROW) return std::nullopt;
    return GraphDefinitionRef{std::string(template_id), std::string(revision),
                              sqlite::column_text(query.get(), 0), sqlite::column_text(query.get(), 1)};
}

StoreResult SQLiteRunStore::schedule_timer(const DurableTimer& timer) {
    std::lock_guard lock(mutex_);
    if(timer.timer_id.empty() || timer.run_id.empty())
        return {StoreStatus::Invalid, 0, "timer and run identities are required"};
    auto* db = sqlite::database(db_);
    sqlite::Statement insert(db, "INSERT INTO durable_timers(timer_id,run_id,due_unix_ms,payload_json)"
                         " VALUES(?,?,?,?)");
    sqlite::bind_text(insert.get(), 1, timer.timer_id); sqlite::bind_text(insert.get(), 2, timer.run_id);
    sqlite3_bind_int64(insert.get(), 3, timer.due_unix_ms);
    sqlite::bind_text(insert.get(), 4, timer.payload.dump());
    const int rc = sqlite3_step(insert.get());
    if(rc == SQLITE_CONSTRAINT) return {StoreStatus::AlreadyExists, 0, "timer already exists"};
    return rc == SQLITE_DONE ? StoreResult{StoreStatus::Committed, 1, {}} : sqlite_failure(db, rc);
}

std::vector<DurableTimer> SQLiteRunStore::claim_due_timers(std::int64_t now_unix_ms,
    std::string_view owner, std::int64_t lease_ms, std::size_t limit) {
    std::lock_guard lock(mutex_);
    std::vector<DurableTimer> result;
    if(owner.empty() || lease_ms <= 0 || limit == 0) return result;
    auto* db = sqlite::database(db_);
    sqlite::Transaction transaction(db);
    sqlite::Statement query(db, "SELECT timer_id,run_id,due_unix_ms,payload_json FROM durable_timers "
                        "WHERE completed=0 AND due_unix_ms<=? AND lease_until_unix_ms<=? "
                        "ORDER BY due_unix_ms,timer_id LIMIT ?");
    sqlite3_bind_int64(query.get(), 1, now_unix_ms);
    sqlite3_bind_int64(query.get(), 2, now_unix_ms);
    sqlite3_bind_int64(query.get(), 3, static_cast<sqlite3_int64>(limit));
    while(sqlite3_step(query.get()) == SQLITE_ROW) {
        result.push_back({sqlite::column_text(query.get(), 0), sqlite::column_text(query.get(), 1),
            sqlite3_column_int64(query.get(), 2), json::parse(sqlite::column_text(query.get(), 3)),
            std::string(owner), now_unix_ms + lease_ms, false});
    }
    sqlite::Statement update(db, "UPDATE durable_timers SET owner=?,lease_until_unix_ms=? "
                         "WHERE timer_id=? AND completed=0 AND lease_until_unix_ms<=?");
    std::vector<DurableTimer> claimed;
    for(const auto& timer : result) {
        sqlite3_reset(update.get()); sqlite3_clear_bindings(update.get());
        sqlite::bind_text(update.get(), 1, owner); sqlite3_bind_int64(update.get(), 2, now_unix_ms + lease_ms);
        sqlite::bind_text(update.get(), 3, timer.timer_id); sqlite3_bind_int64(update.get(), 4, now_unix_ms);
        if(sqlite3_step(update.get()) == SQLITE_DONE && sqlite3_changes(db) == 1) claimed.push_back(timer);
    }
    transaction.commit();
    return claimed;
}

StoreResult SQLiteRunStore::complete_timer(std::string_view timer_id, std::string_view owner) {
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    sqlite::Statement update(db, "UPDATE durable_timers SET completed=1 WHERE timer_id=? AND owner=? AND completed=0");
    sqlite::bind_text(update.get(), 1, timer_id); sqlite::bind_text(update.get(), 2, owner);
    const int rc = sqlite3_step(update.get());
    if(rc != SQLITE_DONE) return sqlite_failure(db, rc);
    if(sqlite3_changes(db) != 1) return {StoreStatus::Invalid, 0, "timer is not owned by caller"};
    return {StoreStatus::Committed, 1, {}};
}

}  // namespace agent_framework::run
