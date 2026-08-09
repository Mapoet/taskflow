#include "agent/planning/cognition_pipeline.hpp"

#include <filesystem>
#include <stdexcept>

#include <sqlite3.h>

namespace agent_framework::planning {
namespace {

using json = nlohmann::json;

sqlite3* database(void* pointer) { return static_cast<sqlite3*>(pointer); }

void execute(sqlite3* db, const char* sql) {
    char* message = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if(rc == SQLITE_OK) return;
    const std::string error = message ? message : sqlite3_errmsg(db);
    sqlite3_free(message);
    throw std::runtime_error(error);
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if(sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(statement_); }
    sqlite3_stmt* get() const { return statement_; }
private:
    sqlite3* db_;
    sqlite3_stmt* statement_{nullptr};
};

class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db) { execute(db_, "BEGIN IMMEDIATE"); }
    ~Transaction() { if(!finished_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() { execute(db_, "COMMIT"); finished_ = true; }
private:
    sqlite3* db_;
    bool finished_{false};
};

std::string column_text(sqlite3_stmt* statement, int column) {
    const auto* text = sqlite3_column_text(statement, column);
    return text ? reinterpret_cast<const char*>(text) : std::string();
}

CognitionCheckpointCommit failure(sqlite3* db, int rc) {
    const auto status = rc == SQLITE_BUSY || rc == SQLITE_LOCKED
        ? CognitionCheckpointStatus::Busy : CognitionCheckpointStatus::Error;
    return {status, 0, {}, sqlite3_errmsg(db)};
}

bool valid(const CognitionCheckpoint& checkpoint, std::string* error) {
    std::vector<contracts::ContractIssue> issues;
    if(!contracts::validate_metadata(checkpoint.metadata, &issues) ||
       checkpoint.pipeline_id.empty() || checkpoint.intake_digest.empty() ||
       checkpoint.revision == 0) {
        if(error) *error = issues.empty()
            ? "pipeline_id, intake_digest and positive revision are required" : issues.front().message;
        return false;
    }
    return true;
}

}  // namespace

SQLiteCognitionCheckpointStore::SQLiteCognitionCheckpointStore(
    std::string path, SQLiteCognitionCheckpointStoreOptions options)
    : path_(std::move(path)), options_(options) {
    if(path_.empty()) throw std::invalid_argument("cognition checkpoint path must not be empty");
    const std::filesystem::path file(path_);
    std::error_code error;
    if(file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), error);
    if(error) throw std::runtime_error("unable to create checkpoint directory: " + error.message());
    sqlite3* opened = nullptr;
    if(sqlite3_open_v2(path_.c_str(), &opened,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        const std::string message = opened ? sqlite3_errmsg(opened) : "sqlite open failed";
        if(opened) sqlite3_close(opened);
        throw std::runtime_error(message);
    }
    db_ = opened;
    sqlite3_busy_timeout(opened, options_.busy_timeout_ms);
    execute(opened, "PRAGMA journal_mode=WAL");
    execute(opened, "PRAGMA synchronous=FULL");
    migrate();
    if(options_.require_private_permissions) {
        std::filesystem::permissions(file,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace, error);
        if(error) throw std::runtime_error("unable to restrict checkpoint permissions: " + error.message());
    }
}

SQLiteCognitionCheckpointStore::~SQLiteCognitionCheckpointStore() {
    if(db_) sqlite3_close(database(db_));
}

void SQLiteCognitionCheckpointStore::migrate() {
    auto* db = database(db_);
    execute(db, "CREATE TABLE IF NOT EXISTS cognition_schema_version("
                "version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)");
    execute(db, "INSERT OR IGNORE INTO cognition_schema_version(version) VALUES(1)");
    execute(db, "CREATE TABLE IF NOT EXISTS cognition_checkpoints("
                "tenant_id TEXT NOT NULL, pipeline_id TEXT NOT NULL, task_id TEXT NOT NULL,"
                "revision INTEGER NOT NULL, state TEXT NOT NULL, checkpoint_json TEXT NOT NULL,"
                "checkpoint_digest TEXT NOT NULL, updated_at TEXT NOT NULL,"
                "PRIMARY KEY(tenant_id,pipeline_id))");
    execute(db, "CREATE INDEX IF NOT EXISTS cognition_task_idx ON cognition_checkpoints("
                "tenant_id,task_id,state)");
}

CognitionCheckpointCommit SQLiteCognitionCheckpointStore::create(
    const CognitionCheckpoint& checkpoint) {
    std::string validation_error;
    if(!valid(checkpoint, &validation_error) || checkpoint.revision != 1)
        return {CognitionCheckpointStatus::Invalid, 0, {}, validation_error.empty()
            ? "initial checkpoint revision must be 1" : validation_error};
    const auto document = encode(checkpoint);
    const auto digest = document.at("canonical_digest").get<std::string>();
    const auto text = document.dump();
    std::lock_guard lock(mutex_);
    auto* db = database(db_);
    Statement statement(db, "INSERT INTO cognition_checkpoints(tenant_id,pipeline_id,task_id,"
        "revision,state,checkpoint_json,checkpoint_digest,updated_at) VALUES(?,?,?,?,?,?,?,?)");
    sqlite3_bind_text(statement.get(), 1, checkpoint.metadata.identity.tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 2, checkpoint.pipeline_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 3, checkpoint.metadata.identity.task_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement.get(), 4, static_cast<sqlite3_int64>(checkpoint.revision));
    const auto state = cognition_pipeline_state_name(checkpoint.state);
    sqlite3_bind_text(statement.get(), 5, state.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 6, text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 7, digest.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 8, checkpoint.updated_at.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(statement.get());
    if(rc == SQLITE_CONSTRAINT) return {CognitionCheckpointStatus::AlreadyExists, 0, {}, {}};
    if(rc != SQLITE_DONE) return failure(db, rc);
    return {CognitionCheckpointStatus::Committed, checkpoint.revision, digest, {}};
}

std::optional<StoredCognitionCheckpoint> SQLiteCognitionCheckpointStore::load(
    std::string_view tenant_id, std::string_view pipeline_id) {
    std::lock_guard lock(mutex_);
    auto* db = database(db_);
    Statement statement(db, "SELECT revision,checkpoint_json,checkpoint_digest FROM "
                            "cognition_checkpoints WHERE tenant_id=? AND pipeline_id=?");
    sqlite3_bind_text(statement.get(), 1, std::string(tenant_id).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 2, std::string(pipeline_id).c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(statement.get());
    if(rc == SQLITE_DONE) return std::nullopt;
    if(rc != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(db));
    const auto revision = static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 0));
    const auto document_text = column_text(statement.get(), 1);
    const auto stored_digest = column_text(statement.get(), 2);
    auto checkpoint = decode_cognition_checkpoint(json::parse(document_text));
    if(!checkpoint || checkpoint->revision != revision ||
       encode(*checkpoint).at("canonical_digest").get<std::string>() != stored_digest)
        throw std::runtime_error("stored cognition checkpoint is corrupt");
    return StoredCognitionCheckpoint{std::move(*checkpoint), revision};
}

CognitionCheckpointCommit SQLiteCognitionCheckpointStore::compare_exchange(
    const CognitionCheckpoint& checkpoint, std::uint64_t expected_revision) {
    std::string validation_error;
    if(!valid(checkpoint, &validation_error) || checkpoint.revision != expected_revision + 1)
        return {CognitionCheckpointStatus::Invalid, 0, {}, validation_error.empty()
            ? "checkpoint revision must advance exactly once" : validation_error};
    const auto document = encode(checkpoint);
    const auto digest = document.at("canonical_digest").get<std::string>();
    const auto text = document.dump();
    std::lock_guard lock(mutex_);
    auto* db = database(db_);
    try {
        Transaction transaction(db);
        Statement statement(db, "UPDATE cognition_checkpoints SET revision=?,state=?,"
            "checkpoint_json=?,checkpoint_digest=?,updated_at=? WHERE tenant_id=? AND "
            "pipeline_id=? AND revision=?");
        sqlite3_bind_int64(statement.get(), 1, static_cast<sqlite3_int64>(checkpoint.revision));
        const auto state = cognition_pipeline_state_name(checkpoint.state);
        sqlite3_bind_text(statement.get(), 2, state.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement.get(), 3, text.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement.get(), 4, digest.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement.get(), 5, checkpoint.updated_at.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement.get(), 6, checkpoint.metadata.identity.tenant_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement.get(), 7, checkpoint.pipeline_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement.get(), 8, static_cast<sqlite3_int64>(expected_revision));
        const int rc = sqlite3_step(statement.get());
        if(rc != SQLITE_DONE) return failure(db, rc);
        if(sqlite3_changes(db) != 1) {
            Statement query(db, "SELECT revision FROM cognition_checkpoints WHERE tenant_id=? AND pipeline_id=?");
            sqlite3_bind_text(query.get(), 1, checkpoint.metadata.identity.tenant_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(query.get(), 2, checkpoint.pipeline_id.c_str(), -1, SQLITE_TRANSIENT);
            const int query_rc = sqlite3_step(query.get());
            if(query_rc == SQLITE_DONE)
                return {CognitionCheckpointStatus::NotFound, 0, {}, {}};
            if(query_rc != SQLITE_ROW) return failure(db, query_rc);
            return {CognitionCheckpointStatus::RevisionConflict,
                    static_cast<std::uint64_t>(sqlite3_column_int64(query.get(), 0)), {}, {}};
        }
        transaction.commit();
        return {CognitionCheckpointStatus::Committed, checkpoint.revision, digest, {}};
    } catch(const std::exception& exception) {
        return {CognitionCheckpointStatus::Error, 0, {}, exception.what()};
    }
}

}  // namespace agent_framework::planning
