#include "agent/memory_v2/workflows/memory_workflow.hpp"

#include <filesystem>
#include <stdexcept>

#include <sqlite3.h>

#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::memory_v2::workflows {
namespace {

using json = nlohmann::json;
namespace sqlite = internal::sqlite;

MemoryWorkflowStoreCommit failure(sqlite3* db, int rc) {
    return {rc == SQLITE_BUSY || rc == SQLITE_LOCKED ? MemoryWorkflowStoreStatus::Busy
                                                      : MemoryWorkflowStoreStatus::Error,
            0, {}, sqlite3_errmsg(db)};
}

bool valid(const MemoryWorkflowCheckpoint& checkpoint, std::string* error) {
    std::vector<contracts::ContractIssue> issues;
    if(!contracts::validate_metadata(checkpoint.metadata, &issues) ||
       checkpoint.workflow_id.empty() || checkpoint.input_digest.empty() ||
       checkpoint.revision == 0) {
        if(error) *error = !issues.empty() ? issues.front().message : "invalid memory workflow checkpoint";
        return false;
    }
    return true;
}

}  // namespace

SQLiteMemoryWorkflowCheckpointStore::SQLiteMemoryWorkflowCheckpointStore(
    std::string path, SQLiteMemoryWorkflowCheckpointStoreOptions options)
    : path_(std::move(path)), options_(options) {
    if(path_.empty()) throw std::invalid_argument("memory workflow database path is required");
    const auto parent = std::filesystem::path(path_).parent_path();
    if(!parent.empty()) std::filesystem::create_directories(parent);
    sqlite3* handle = nullptr;
    if(sqlite3_open_v2(path_.c_str(), &handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                       SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        const std::string error = handle ? sqlite3_errmsg(handle) : "sqlite open failed";
        if(handle) sqlite3_close(handle);
        throw std::runtime_error(error);
    }
    db_ = handle;
    sqlite3_busy_timeout(handle, options_.busy_timeout_ms);
    sqlite::exec(handle, "PRAGMA journal_mode=WAL");
    sqlite::exec(handle, "PRAGMA synchronous=FULL");
    sqlite::exec(handle, "PRAGMA foreign_keys=ON");
    migrate();
    if(options_.require_private_permissions) {
        std::error_code error;
        std::filesystem::permissions(path_, std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, error);
        if(error) throw std::runtime_error("cannot set private memory workflow database permissions");
    }
}

SQLiteMemoryWorkflowCheckpointStore::~SQLiteMemoryWorkflowCheckpointStore() {
    if(db_) sqlite3_close(sqlite::database(db_));
}

void SQLiteMemoryWorkflowCheckpointStore::migrate() {
    sqlite::exec(sqlite::database(db_),
        "CREATE TABLE IF NOT EXISTS memory_workflow_schema(version INTEGER NOT NULL)");
    sqlite::exec(sqlite::database(db_),
        "INSERT INTO memory_workflow_schema(version) "
        "SELECT 1 WHERE NOT EXISTS(SELECT 1 FROM memory_workflow_schema)");
    sqlite::exec(sqlite::database(db_),
        "CREATE TABLE IF NOT EXISTS memory_workflow_checkpoints("
        "tenant_id TEXT NOT NULL, workflow_id TEXT NOT NULL, revision INTEGER NOT NULL, "
        "document TEXT NOT NULL, digest TEXT NOT NULL, updated_at TEXT NOT NULL, "
        "PRIMARY KEY(tenant_id, workflow_id))");
}

MemoryWorkflowStoreCommit SQLiteMemoryWorkflowCheckpointStore::create(
    const MemoryWorkflowCheckpoint& checkpoint) {
    std::string validation_error;
    if(!valid(checkpoint, &validation_error) || checkpoint.revision != 1)
        return {MemoryWorkflowStoreStatus::Invalid, 0, {},
                validation_error.empty() ? "initial checkpoint revision must be 1" : validation_error};
    const auto document = encode(checkpoint);
    const auto digest = document.at("canonical_digest").get<std::string>();
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    try {
        sqlite::Statement statement(db,
            "INSERT INTO memory_workflow_checkpoints"
            "(tenant_id,workflow_id,revision,document,digest,updated_at) VALUES(?,?,?,?,?,?)");
        sqlite::bind_text(statement.get(), 1, checkpoint.metadata.identity.tenant_id);
        sqlite::bind_text(statement.get(), 2, checkpoint.workflow_id);
        sqlite3_bind_int64(statement.get(), 3, 1);
        sqlite::bind_text(statement.get(), 4, document.dump());
        sqlite::bind_text(statement.get(), 5, digest);
        sqlite::bind_text(statement.get(), 6, checkpoint.updated_at);
        const int rc = sqlite3_step(statement.get());
        if(rc == SQLITE_CONSTRAINT)
            return {MemoryWorkflowStoreStatus::AlreadyExists, 0, {}, {}};
        if(rc != SQLITE_DONE) return failure(db, rc);
        return {MemoryWorkflowStoreStatus::Committed, 1, digest, {}};
    } catch(const std::exception& exception) {
        return {MemoryWorkflowStoreStatus::Error, 0, {}, exception.what()};
    }
}

std::optional<StoredMemoryWorkflowCheckpoint> SQLiteMemoryWorkflowCheckpointStore::load(
    std::string_view tenant_id, std::string_view workflow_id) {
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    try {
        sqlite::Statement statement(db,
            "SELECT revision,document,digest FROM memory_workflow_checkpoints "
            "WHERE tenant_id=? AND workflow_id=?");
        sqlite::bind_text(statement.get(), 1, tenant_id);
        sqlite::bind_text(statement.get(), 2, workflow_id);
        if(sqlite3_step(statement.get()) != SQLITE_ROW) return std::nullopt;
        const auto revision = static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 0));
        const auto document_text = sqlite::column_text(statement.get(), 1);
        const auto stored_digest = sqlite::column_text(statement.get(), 2);
        const auto document = json::parse(document_text);
        if(document.at("canonical_digest").get<std::string>() != stored_digest)
            throw std::runtime_error("memory workflow checkpoint digest mismatch");
        std::vector<contracts::ContractIssue> issues;
        auto checkpoint = decode_memory_workflow_checkpoint(document, {}, &issues);
        if(!checkpoint || checkpoint->revision != revision)
            throw std::runtime_error(issues.empty() ? "invalid memory workflow checkpoint"
                                                    : issues.front().message);
        return StoredMemoryWorkflowCheckpoint{std::move(*checkpoint), revision};
    } catch(...) {
        return std::nullopt;
    }
}

MemoryWorkflowStoreCommit SQLiteMemoryWorkflowCheckpointStore::compare_exchange(
    const MemoryWorkflowCheckpoint& checkpoint, std::uint64_t expected_revision) {
    std::string validation_error;
    if(!valid(checkpoint, &validation_error) || checkpoint.revision != expected_revision + 1)
        return {MemoryWorkflowStoreStatus::Invalid, 0, {},
                validation_error.empty() ? "checkpoint revision must advance exactly once"
                                         : validation_error};
    const auto document = encode(checkpoint);
    const auto digest = document.at("canonical_digest").get<std::string>();
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    try {
        sqlite::Transaction transaction(db);
        sqlite::Statement statement(db,
            "UPDATE memory_workflow_checkpoints SET revision=?,document=?,digest=?,updated_at=? "
            "WHERE tenant_id=? AND workflow_id=? AND revision=?");
        sqlite3_bind_int64(statement.get(), 1, static_cast<sqlite3_int64>(checkpoint.revision));
        sqlite::bind_text(statement.get(), 2, document.dump());
        sqlite::bind_text(statement.get(), 3, digest);
        sqlite::bind_text(statement.get(), 4, checkpoint.updated_at);
        sqlite::bind_text(statement.get(), 5, checkpoint.metadata.identity.tenant_id);
        sqlite::bind_text(statement.get(), 6, checkpoint.workflow_id);
        sqlite3_bind_int64(statement.get(), 7, static_cast<sqlite3_int64>(expected_revision));
        const int rc = sqlite3_step(statement.get());
        if(rc != SQLITE_DONE) return failure(db, rc);
        if(sqlite3_changes(db) != 1) {
            sqlite::Statement current(db,
                "SELECT revision FROM memory_workflow_checkpoints WHERE tenant_id=? AND workflow_id=?");
            sqlite::bind_text(current.get(), 1, checkpoint.metadata.identity.tenant_id);
            sqlite::bind_text(current.get(), 2, checkpoint.workflow_id);
            if(sqlite3_step(current.get()) != SQLITE_ROW)
                return {MemoryWorkflowStoreStatus::NotFound, 0, {}, {}};
            return {MemoryWorkflowStoreStatus::RevisionConflict,
                    static_cast<std::uint64_t>(sqlite3_column_int64(current.get(), 0)), {}, {}};
        }
        transaction.commit();
        return {MemoryWorkflowStoreStatus::Committed, checkpoint.revision, digest, {}};
    } catch(const std::exception& exception) {
        return {MemoryWorkflowStoreStatus::Error, 0, {}, exception.what()};
    }
}

}  // namespace agent_framework::memory_v2::workflows
