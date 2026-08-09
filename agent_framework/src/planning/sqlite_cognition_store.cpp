#include "agent/planning/cognition_pipeline.hpp"

#include <filesystem>
#include <stdexcept>

#include <sqlite3.h>

#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::planning {
namespace {

using json = nlohmann::json;
namespace sqlite = internal::sqlite;

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
    sqlite::exec(opened, "PRAGMA journal_mode=WAL");
    sqlite::exec(opened, "PRAGMA synchronous=FULL");
    migrate();
    if(options_.require_private_permissions) {
        std::filesystem::permissions(file,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace, error);
        if(error) throw std::runtime_error("unable to restrict checkpoint permissions: " + error.message());
    }
}

SQLiteCognitionCheckpointStore::~SQLiteCognitionCheckpointStore() {
    if(db_) sqlite3_close(sqlite::database(db_));
}

void SQLiteCognitionCheckpointStore::migrate() {
    auto* db = sqlite::database(db_);
    sqlite::exec(db, "CREATE TABLE IF NOT EXISTS cognition_schema_version("
                "version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)");
    sqlite::exec(db, "INSERT OR IGNORE INTO cognition_schema_version(version) VALUES(1)");
    sqlite::exec(db, "CREATE TABLE IF NOT EXISTS cognition_checkpoints("
                "tenant_id TEXT NOT NULL, pipeline_id TEXT NOT NULL, task_id TEXT NOT NULL,"
                "revision INTEGER NOT NULL, state TEXT NOT NULL, checkpoint_json TEXT NOT NULL,"
                "checkpoint_digest TEXT NOT NULL, updated_at TEXT NOT NULL,"
                "PRIMARY KEY(tenant_id,pipeline_id))");
    sqlite::exec(db, "CREATE INDEX IF NOT EXISTS cognition_task_idx ON cognition_checkpoints("
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
    auto* db = sqlite::database(db_);
    sqlite::Statement statement(db, "INSERT INTO cognition_checkpoints(tenant_id,pipeline_id,task_id,"
        "revision,state,checkpoint_json,checkpoint_digest,updated_at) VALUES(?,?,?,?,?,?,?,?)");
    sqlite::bind_text(statement.get(), 1, checkpoint.metadata.identity.tenant_id);
    sqlite::bind_text(statement.get(), 2, checkpoint.pipeline_id);
    sqlite::bind_text(statement.get(), 3, checkpoint.metadata.identity.task_id);
    sqlite3_bind_int64(statement.get(), 4, static_cast<sqlite3_int64>(checkpoint.revision));
    const auto state = cognition_pipeline_state_name(checkpoint.state);
    sqlite::bind_text(statement.get(), 5, state);
    sqlite::bind_text(statement.get(), 6, text);
    sqlite::bind_text(statement.get(), 7, digest);
    sqlite::bind_text(statement.get(), 8, checkpoint.updated_at);
    const int rc = sqlite3_step(statement.get());
    if(rc == SQLITE_CONSTRAINT) return {CognitionCheckpointStatus::AlreadyExists, 0, {}, {}};
    if(rc != SQLITE_DONE) return failure(db, rc);
    return {CognitionCheckpointStatus::Committed, checkpoint.revision, digest, {}};
}

std::optional<StoredCognitionCheckpoint> SQLiteCognitionCheckpointStore::load(
    std::string_view tenant_id, std::string_view pipeline_id) {
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    sqlite::Statement statement(db, "SELECT revision,checkpoint_json,checkpoint_digest FROM "
                            "cognition_checkpoints WHERE tenant_id=? AND pipeline_id=?");
    sqlite::bind_text(statement.get(), 1, tenant_id);
    sqlite::bind_text(statement.get(), 2, pipeline_id);
    const int rc = sqlite3_step(statement.get());
    if(rc == SQLITE_DONE) return std::nullopt;
    if(rc != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(db));
    const auto revision = static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 0));
    const auto document_text = sqlite::column_text(statement.get(), 1);
    const auto stored_digest = sqlite::column_text(statement.get(), 2);
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
    auto* db = sqlite::database(db_);
    try {
        sqlite::Transaction transaction(db);
        sqlite::Statement statement(db, "UPDATE cognition_checkpoints SET revision=?,state=?,"
            "checkpoint_json=?,checkpoint_digest=?,updated_at=? WHERE tenant_id=? AND "
            "pipeline_id=? AND revision=?");
        sqlite3_bind_int64(statement.get(), 1, static_cast<sqlite3_int64>(checkpoint.revision));
        const auto state = cognition_pipeline_state_name(checkpoint.state);
        sqlite::bind_text(statement.get(), 2, state);
        sqlite::bind_text(statement.get(), 3, text);
        sqlite::bind_text(statement.get(), 4, digest);
        sqlite::bind_text(statement.get(), 5, checkpoint.updated_at);
        sqlite::bind_text(statement.get(), 6, checkpoint.metadata.identity.tenant_id);
        sqlite::bind_text(statement.get(), 7, checkpoint.pipeline_id);
        sqlite3_bind_int64(statement.get(), 8, static_cast<sqlite3_int64>(expected_revision));
        const int rc = sqlite3_step(statement.get());
        if(rc != SQLITE_DONE) return failure(db, rc);
        if(sqlite3_changes(db) != 1) {
            sqlite::Statement query(db, "SELECT revision FROM cognition_checkpoints WHERE tenant_id=? AND pipeline_id=?");
            sqlite::bind_text(query.get(), 1, checkpoint.metadata.identity.tenant_id);
            sqlite::bind_text(query.get(), 2, checkpoint.pipeline_id);
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
