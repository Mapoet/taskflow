#include "agent/remediation/remediation_workflow.hpp"

#include <filesystem>
#include <stdexcept>

#include <sqlite3.h>

#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::remediation {
namespace {
namespace sqlite = internal::sqlite;

bool valid(const RemediationCheckpoint& v, std::string* error) {
    if(v.metadata.identity.tenant_id.empty() || v.metadata.identity.task_id.empty() ||
       v.workflow_id.empty() || v.revision == 0 || v.current_plan_digest.empty() ||
       v.acceptance_contract_digest.empty() || v.acceptance_report_digest.empty() ||
       v.assurance_checkpoint_digest.empty() || v.impact_inventory_digest.empty()) {
        if(error) *error = "identity, workflow, revision and all immutable input digests are required";
        return false;
    }
    return true;
}
RemediationStoreCommit failure(sqlite3* db, int rc) {
    return {rc == SQLITE_BUSY || rc == SQLITE_LOCKED ? RemediationStoreStatus::Busy
                                                      : RemediationStoreStatus::Error,
            0, {}, sqlite3_errmsg(db)};
}
}  // namespace

std::string InMemoryRemediationStore::key(std::string_view tenant, std::string_view workflow) {
    return std::string(tenant) + '\n' + std::string(workflow);
}
RemediationStoreCommit InMemoryRemediationStore::create(const RemediationCheckpoint& v) {
    std::string error;
    if(!valid(v, &error) || v.revision != 1)
        return {RemediationStoreStatus::Invalid, 0, {}, error.empty() ? "initial revision must be 1" : error};
    std::lock_guard lock(mutex_);
    const auto id = key(v.metadata.identity.tenant_id, v.workflow_id);
    if(values_.count(id)) return {RemediationStoreStatus::AlreadyExists, 0, {}, {}};
    values_[id] = {v, v.revision};
    return {RemediationStoreStatus::Committed, v.revision,
            encode(v).at("canonical_digest").get<std::string>(), {}};
}
std::optional<StoredRemediationCheckpoint> InMemoryRemediationStore::load(
    std::string_view tenant, std::string_view workflow) {
    std::lock_guard lock(mutex_);
    auto found = values_.find(key(tenant, workflow));
    return found == values_.end() ? std::nullopt : std::optional(found->second);
}
RemediationStoreCommit InMemoryRemediationStore::compare_exchange(
    const RemediationCheckpoint& v, std::uint64_t expected) {
    std::string error;
    if(!valid(v, &error) || v.revision != expected + 1)
        return {RemediationStoreStatus::Invalid, 0, {}, error.empty() ? "revision must advance once" : error};
    std::lock_guard lock(mutex_);
    auto found = values_.find(key(v.metadata.identity.tenant_id, v.workflow_id));
    if(found == values_.end()) return {RemediationStoreStatus::NotFound, 0, {}, {}};
    if(found->second.revision != expected)
        return {RemediationStoreStatus::RevisionConflict, found->second.revision, {}, {}};
    found->second = {v, v.revision};
    return {RemediationStoreStatus::Committed, v.revision,
            encode(v).at("canonical_digest").get<std::string>(), {}};
}

SQLiteRemediationStore::SQLiteRemediationStore(std::string path, SQLiteRemediationStoreOptions options)
    : path_(std::move(path)), options_(options) {
    if(path_.empty()) throw std::invalid_argument("remediation store path must not be empty");
    std::filesystem::path file(path_);
    std::error_code error;
    if(file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), error);
    if(error) throw std::runtime_error(error.message());
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
#if !defined(_WIN32)
    if(options_.require_private_permissions) {
        std::filesystem::permissions(file,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace, error);
        if(error) throw std::runtime_error(error.message());
    }
#endif
}
SQLiteRemediationStore::~SQLiteRemediationStore() {
    if(db_) sqlite3_close(sqlite::database(db_));
}
void SQLiteRemediationStore::migrate() {
    auto* db = sqlite::database(db_);
    sqlite::exec(db, "CREATE TABLE IF NOT EXISTS remediation_schema_version("
                     "version INTEGER PRIMARY KEY,applied_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)");
    sqlite::exec(db, "INSERT OR IGNORE INTO remediation_schema_version(version) VALUES(1)");
    sqlite::exec(db, "CREATE TABLE IF NOT EXISTS remediation_checkpoints("
                     "tenant_id TEXT NOT NULL,workflow_id TEXT NOT NULL,task_id TEXT NOT NULL,"
                     "revision INTEGER NOT NULL,state TEXT NOT NULL,checkpoint_json TEXT NOT NULL,"
                     "checkpoint_digest TEXT NOT NULL,updated_at TEXT NOT NULL,"
                     "PRIMARY KEY(tenant_id,workflow_id))");
    sqlite::exec(db, "CREATE INDEX IF NOT EXISTS remediation_task_idx ON remediation_checkpoints(tenant_id,task_id,state)");
}
RemediationStoreCommit SQLiteRemediationStore::create(const RemediationCheckpoint& v) {
    std::string error;
    if(!valid(v, &error) || v.revision != 1)
        return {RemediationStoreStatus::Invalid, 0, {}, error.empty() ? "initial revision must be 1" : error};
    const auto document = encode(v);
    const auto digest = document.at("canonical_digest").get<std::string>();
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    sqlite::Statement statement(db, "INSERT INTO remediation_checkpoints(tenant_id,workflow_id,task_id,revision,state,checkpoint_json,checkpoint_digest,updated_at) VALUES(?,?,?,?,?,?,?,?)");
    sqlite::bind_text(statement.get(), 1, v.metadata.identity.tenant_id);
    sqlite::bind_text(statement.get(), 2, v.workflow_id);
    sqlite::bind_text(statement.get(), 3, v.metadata.identity.task_id);
    sqlite3_bind_int64(statement.get(), 4, static_cast<sqlite3_int64>(v.revision));
    sqlite::bind_text(statement.get(), 5, remediation_state_name(v.state));
    sqlite::bind_text(statement.get(), 6, document.dump());
    sqlite::bind_text(statement.get(), 7, digest);
    sqlite::bind_text(statement.get(), 8, v.updated_at);
    const int rc = sqlite3_step(statement.get());
    if(rc == SQLITE_CONSTRAINT) return {RemediationStoreStatus::AlreadyExists, 0, {}, {}};
    if(rc != SQLITE_DONE) return failure(db, rc);
    return {RemediationStoreStatus::Committed, v.revision, digest, {}};
}
std::optional<StoredRemediationCheckpoint> SQLiteRemediationStore::load(
    std::string_view tenant, std::string_view workflow) {
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    sqlite::Statement statement(db, "SELECT revision,checkpoint_json,checkpoint_digest FROM remediation_checkpoints WHERE tenant_id=? AND workflow_id=?");
    sqlite::bind_text(statement.get(), 1, tenant);
    sqlite::bind_text(statement.get(), 2, workflow);
    const int rc = sqlite3_step(statement.get());
    if(rc == SQLITE_DONE) return std::nullopt;
    if(rc != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(db));
    const auto revision = static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 0));
    const auto text = sqlite::column_text(statement.get(), 1);
    const auto stored_digest = sqlite::column_text(statement.get(), 2);
    auto value = decode_remediation_checkpoint(nlohmann::json::parse(text));
    if(!value || value->revision != revision ||
       encode(*value).at("canonical_digest").get<std::string>() != stored_digest)
        throw std::runtime_error("stored remediation checkpoint is corrupt");
    return StoredRemediationCheckpoint{std::move(*value), revision};
}
RemediationStoreCommit SQLiteRemediationStore::compare_exchange(
    const RemediationCheckpoint& v, std::uint64_t expected) {
    std::string error;
    if(!valid(v, &error) || v.revision != expected + 1)
        return {RemediationStoreStatus::Invalid, 0, {}, error.empty() ? "revision must advance once" : error};
    const auto document = encode(v);
    const auto digest = document.at("canonical_digest").get<std::string>();
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    sqlite::Statement statement(db, "UPDATE remediation_checkpoints SET revision=?,state=?,checkpoint_json=?,checkpoint_digest=?,updated_at=? WHERE tenant_id=? AND workflow_id=? AND revision=?");
    sqlite3_bind_int64(statement.get(), 1, static_cast<sqlite3_int64>(v.revision));
    sqlite::bind_text(statement.get(), 2, remediation_state_name(v.state));
    sqlite::bind_text(statement.get(), 3, document.dump());
    sqlite::bind_text(statement.get(), 4, digest);
    sqlite::bind_text(statement.get(), 5, v.updated_at);
    sqlite::bind_text(statement.get(), 6, v.metadata.identity.tenant_id);
    sqlite::bind_text(statement.get(), 7, v.workflow_id);
    sqlite3_bind_int64(statement.get(), 8, static_cast<sqlite3_int64>(expected));
    const int rc = sqlite3_step(statement.get());
    if(rc != SQLITE_DONE) return failure(db, rc);
    if(sqlite3_changes(db) != 1) {
        sqlite::Statement query(db, "SELECT revision FROM remediation_checkpoints WHERE tenant_id=? AND workflow_id=?");
        sqlite::bind_text(query.get(), 1, v.metadata.identity.tenant_id);
        sqlite::bind_text(query.get(), 2, v.workflow_id);
        const int qrc = sqlite3_step(query.get());
        if(qrc == SQLITE_DONE) return {RemediationStoreStatus::NotFound, 0, {}, {}};
        if(qrc != SQLITE_ROW) return failure(db, qrc);
        return {RemediationStoreStatus::RevisionConflict,
                static_cast<std::uint64_t>(sqlite3_column_int64(query.get(), 0)), {}, {}};
    }
    return {RemediationStoreStatus::Committed, v.revision, digest, {}};
}

}  // namespace agent_framework::remediation
