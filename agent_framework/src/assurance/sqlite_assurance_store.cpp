#include "agent/assurance/professional_workflow.hpp"

#include <filesystem>
#include <stdexcept>

#include <sqlite3.h>

#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::assurance {
namespace {

using json = nlohmann::json;
namespace sqlite = internal::sqlite;

bool valid_checkpoint(const AssuranceCheckpoint& value, std::string* error) {
    std::vector<contracts::ContractIssue> issues;
    if(!contracts::validate_metadata(value.metadata, &issues) || value.workflow_id.empty() ||
       value.revision == 0 || value.acceptance_contract_digest.empty() ||
       value.task_context_digest.empty() || value.artifact_manifest_digest.empty()) {
        if(error) *error = issues.empty()
            ? "workflow, positive revision and bound input digests are required"
            : issues.front().message;
        return false;
    }
    return true;
}

bool valid_terminal(const AssuranceCheckpoint& checkpoint, const AcceptanceReport& report,
                    std::string* error) {
    if(!valid_checkpoint(checkpoint, error)) return false;
    if(checkpoint.state != AssuranceWorkflowState::Completed &&
       checkpoint.state != AssuranceWorkflowState::ManualReview) {
        if(error) *error = "report commit requires a terminal assurance checkpoint";
        return false;
    }
    if(report.metadata.identity.tenant_id != checkpoint.metadata.identity.tenant_id ||
       report.metadata.identity.task_id != checkpoint.metadata.identity.task_id ||
       report.acceptance_contract_digest != checkpoint.acceptance_contract_digest ||
       report.plan_digest.empty()) {
        if(error) *error = "report identity or contract binding does not match checkpoint";
        return false;
    }
    const auto digest = encode(report).at("canonical_digest").get<std::string>();
    if(checkpoint.acceptance_report_digest != digest) {
        if(error) *error = "checkpoint report digest does not match report";
        return false;
    }
    return true;
}

AssuranceStoreCommit failure(sqlite3* db, int rc) {
    return {rc == SQLITE_BUSY || rc == SQLITE_LOCKED ? AssuranceStoreStatus::Busy
                                                      : AssuranceStoreStatus::Error,
            0, {}, sqlite3_errmsg(db)};
}

}  // namespace

std::string InMemoryAssuranceStore::key(std::string_view tenant_id,
                                        std::string_view workflow_id) {
    return std::string(tenant_id) + "\n" + std::string(workflow_id);
}

AssuranceStoreCommit InMemoryAssuranceStore::create_checkpoint(
    const AssuranceCheckpoint& checkpoint) {
    std::string error;
    if(!valid_checkpoint(checkpoint, &error) || checkpoint.revision != 1)
        return {AssuranceStoreStatus::Invalid, 0, {},
                error.empty() ? "initial checkpoint revision must be 1" : error};
    std::lock_guard lock(mutex_);
    const auto id = key(checkpoint.metadata.identity.tenant_id, checkpoint.workflow_id);
    if(checkpoints_.count(id)) return {AssuranceStoreStatus::AlreadyExists, 0, {}, {}};
    checkpoints_.emplace(id, StoredAssuranceCheckpoint{checkpoint, checkpoint.revision});
    return {AssuranceStoreStatus::Committed, checkpoint.revision,
            encode(checkpoint).at("canonical_digest").get<std::string>(), {}};
}

std::optional<StoredAssuranceCheckpoint> InMemoryAssuranceStore::load_checkpoint(
    std::string_view tenant_id, std::string_view workflow_id) {
    std::lock_guard lock(mutex_);
    const auto found = checkpoints_.find(key(tenant_id, workflow_id));
    return found == checkpoints_.end() ? std::nullopt
                                       : std::optional<StoredAssuranceCheckpoint>(found->second);
}

AssuranceStoreCommit InMemoryAssuranceStore::compare_exchange_checkpoint(
    const AssuranceCheckpoint& checkpoint, std::uint64_t expected_revision) {
    std::string error;
    if(!valid_checkpoint(checkpoint, &error) || checkpoint.revision != expected_revision + 1)
        return {AssuranceStoreStatus::Invalid, 0, {},
                error.empty() ? "checkpoint revision must advance exactly once" : error};
    std::lock_guard lock(mutex_);
    const auto id = key(checkpoint.metadata.identity.tenant_id, checkpoint.workflow_id);
    const auto found = checkpoints_.find(id);
    if(found == checkpoints_.end()) return {AssuranceStoreStatus::NotFound, 0, {}, {}};
    if(found->second.revision != expected_revision)
        return {AssuranceStoreStatus::RevisionConflict, found->second.revision, {}, {}};
    found->second = {checkpoint, checkpoint.revision};
    return {AssuranceStoreStatus::Committed, checkpoint.revision,
            encode(checkpoint).at("canonical_digest").get<std::string>(), {}};
}

AssuranceStoreCommit InMemoryAssuranceStore::commit_report(
    const AssuranceCheckpoint& checkpoint, std::uint64_t expected_revision,
    const AcceptanceReport& report) {
    std::string error;
    if(!valid_terminal(checkpoint, report, &error) || checkpoint.revision != expected_revision + 1)
        return {AssuranceStoreStatus::Invalid, 0, {},
                error.empty() ? "terminal checkpoint revision must advance exactly once" : error};
    std::lock_guard lock(mutex_);
    const auto id = key(checkpoint.metadata.identity.tenant_id, checkpoint.workflow_id);
    const auto found = checkpoints_.find(id);
    if(found == checkpoints_.end()) return {AssuranceStoreStatus::NotFound, 0, {}, {}};
    if(found->second.revision != expected_revision)
        return {AssuranceStoreStatus::RevisionConflict, found->second.revision, {}, {}};
    const auto existing = reports_.find(id);
    if(existing != reports_.end()) {
        const auto existing_digest = encode(existing->second.report).at("canonical_digest").get<std::string>();
        if(existing_digest == checkpoint.acceptance_report_digest)
            return {AssuranceStoreStatus::AlreadyExists, existing->second.revision,
                    existing_digest, {}};
        return {AssuranceStoreStatus::RevisionConflict, existing->second.revision, {},
                "a different terminal report already exists"};
    }
    found->second = {checkpoint, checkpoint.revision};
    reports_.emplace(id, StoredAcceptanceReport{report, checkpoint.revision, checkpoint.workflow_id});
    return {AssuranceStoreStatus::Committed, checkpoint.revision,
            checkpoint.acceptance_report_digest, {}};
}

std::optional<StoredAcceptanceReport> InMemoryAssuranceStore::load_report(
    std::string_view tenant_id, std::string_view workflow_id) {
    std::lock_guard lock(mutex_);
    const auto found = reports_.find(key(tenant_id, workflow_id));
    return found == reports_.end() ? std::nullopt
                                   : std::optional<StoredAcceptanceReport>(found->second);
}

SQLiteAssuranceStore::SQLiteAssuranceStore(std::string path, SQLiteAssuranceStoreOptions options)
    : path_(std::move(path)), options_(options) {
    if(path_.empty()) throw std::invalid_argument("assurance store path must not be empty");
    const std::filesystem::path file(path_);
    std::error_code error;
    if(file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), error);
    if(error) throw std::runtime_error("unable to create assurance store directory: " + error.message());
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
        if(error) throw std::runtime_error("unable to restrict assurance store permissions: " + error.message());
    }
}

SQLiteAssuranceStore::~SQLiteAssuranceStore() {
    if(db_) sqlite3_close(sqlite::database(db_));
}

void SQLiteAssuranceStore::migrate() {
    auto* db = sqlite::database(db_);
    sqlite::exec(db, "CREATE TABLE IF NOT EXISTS assurance_schema_version("
                     "version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)");
    sqlite::exec(db, "INSERT OR IGNORE INTO assurance_schema_version(version) VALUES(1)");
    sqlite::exec(db, "CREATE TABLE IF NOT EXISTS assurance_checkpoints("
                     "tenant_id TEXT NOT NULL, workflow_id TEXT NOT NULL, task_id TEXT NOT NULL,"
                     "revision INTEGER NOT NULL, state TEXT NOT NULL, checkpoint_json TEXT NOT NULL,"
                     "checkpoint_digest TEXT NOT NULL, updated_at TEXT NOT NULL,"
                     "PRIMARY KEY(tenant_id,workflow_id))");
    sqlite::exec(db, "CREATE TABLE IF NOT EXISTS acceptance_reports("
                     "tenant_id TEXT NOT NULL, workflow_id TEXT NOT NULL, task_id TEXT NOT NULL,"
                     "revision INTEGER NOT NULL, report_json TEXT NOT NULL, report_digest TEXT NOT NULL,"
                     "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                     "PRIMARY KEY(tenant_id,workflow_id))");
    sqlite::exec(db, "CREATE INDEX IF NOT EXISTS assurance_task_idx ON assurance_checkpoints("
                     "tenant_id,task_id,state)");
}

AssuranceStoreCommit SQLiteAssuranceStore::create_checkpoint(
    const AssuranceCheckpoint& checkpoint) {
    std::string error;
    if(!valid_checkpoint(checkpoint, &error) || checkpoint.revision != 1)
        return {AssuranceStoreStatus::Invalid, 0, {},
                error.empty() ? "initial checkpoint revision must be 1" : error};
    const auto document = encode(checkpoint);
    const auto digest = document.at("canonical_digest").get<std::string>();
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    sqlite::Statement statement(db, "INSERT INTO assurance_checkpoints(tenant_id,workflow_id,"
        "task_id,revision,state,checkpoint_json,checkpoint_digest,updated_at) VALUES(?,?,?,?,?,?,?,?)");
    sqlite::bind_text(statement.get(), 1, checkpoint.metadata.identity.tenant_id);
    sqlite::bind_text(statement.get(), 2, checkpoint.workflow_id);
    sqlite::bind_text(statement.get(), 3, checkpoint.metadata.identity.task_id);
    sqlite3_bind_int64(statement.get(), 4, static_cast<sqlite3_int64>(checkpoint.revision));
    sqlite::bind_text(statement.get(), 5, assurance_workflow_state_name(checkpoint.state));
    sqlite::bind_text(statement.get(), 6, document.dump());
    sqlite::bind_text(statement.get(), 7, digest);
    sqlite::bind_text(statement.get(), 8, checkpoint.updated_at);
    const int rc = sqlite3_step(statement.get());
    if(rc == SQLITE_CONSTRAINT) return {AssuranceStoreStatus::AlreadyExists, 0, {}, {}};
    if(rc != SQLITE_DONE) return failure(db, rc);
    return {AssuranceStoreStatus::Committed, checkpoint.revision, digest, {}};
}

std::optional<StoredAssuranceCheckpoint> SQLiteAssuranceStore::load_checkpoint(
    std::string_view tenant_id, std::string_view workflow_id) {
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    sqlite::Statement statement(db, "SELECT revision,checkpoint_json,checkpoint_digest FROM "
                                    "assurance_checkpoints WHERE tenant_id=? AND workflow_id=?");
    sqlite::bind_text(statement.get(), 1, tenant_id);
    sqlite::bind_text(statement.get(), 2, workflow_id);
    const int rc = sqlite3_step(statement.get());
    if(rc == SQLITE_DONE) return std::nullopt;
    if(rc != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(db));
    const auto revision = static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 0));
    const auto document_text = sqlite::column_text(statement.get(), 1);
    const auto stored_digest = sqlite::column_text(statement.get(), 2);
    auto checkpoint = decode_assurance_checkpoint(json::parse(document_text));
    if(!checkpoint || checkpoint->revision != revision ||
       encode(*checkpoint).at("canonical_digest").get<std::string>() != stored_digest)
        throw std::runtime_error("stored assurance checkpoint is corrupt");
    return StoredAssuranceCheckpoint{std::move(*checkpoint), revision};
}

AssuranceStoreCommit SQLiteAssuranceStore::compare_exchange_checkpoint(
    const AssuranceCheckpoint& checkpoint, std::uint64_t expected_revision) {
    std::string error;
    if(!valid_checkpoint(checkpoint, &error) || checkpoint.revision != expected_revision + 1)
        return {AssuranceStoreStatus::Invalid, 0, {},
                error.empty() ? "checkpoint revision must advance exactly once" : error};
    const auto document = encode(checkpoint);
    const auto digest = document.at("canonical_digest").get<std::string>();
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    sqlite::Statement statement(db, "UPDATE assurance_checkpoints SET revision=?,state=?,"
        "checkpoint_json=?,checkpoint_digest=?,updated_at=? WHERE tenant_id=? AND workflow_id=? AND revision=?");
    sqlite3_bind_int64(statement.get(), 1, static_cast<sqlite3_int64>(checkpoint.revision));
    sqlite::bind_text(statement.get(), 2, assurance_workflow_state_name(checkpoint.state));
    sqlite::bind_text(statement.get(), 3, document.dump());
    sqlite::bind_text(statement.get(), 4, digest);
    sqlite::bind_text(statement.get(), 5, checkpoint.updated_at);
    sqlite::bind_text(statement.get(), 6, checkpoint.metadata.identity.tenant_id);
    sqlite::bind_text(statement.get(), 7, checkpoint.workflow_id);
    sqlite3_bind_int64(statement.get(), 8, static_cast<sqlite3_int64>(expected_revision));
    const int rc = sqlite3_step(statement.get());
    if(rc != SQLITE_DONE) return failure(db, rc);
    if(sqlite3_changes(db) != 1) {
        sqlite::Statement query(db, "SELECT revision FROM assurance_checkpoints WHERE tenant_id=? AND workflow_id=?");
        sqlite::bind_text(query.get(), 1, checkpoint.metadata.identity.tenant_id);
        sqlite::bind_text(query.get(), 2, checkpoint.workflow_id);
        const int query_rc = sqlite3_step(query.get());
        if(query_rc == SQLITE_DONE) return {AssuranceStoreStatus::NotFound, 0, {}, {}};
        if(query_rc != SQLITE_ROW) return failure(db, query_rc);
        return {AssuranceStoreStatus::RevisionConflict,
                static_cast<std::uint64_t>(sqlite3_column_int64(query.get(), 0)), {}, {}};
    }
    return {AssuranceStoreStatus::Committed, checkpoint.revision, digest, {}};
}

AssuranceStoreCommit SQLiteAssuranceStore::commit_report(
    const AssuranceCheckpoint& checkpoint, std::uint64_t expected_revision,
    const AcceptanceReport& report) {
    std::string error;
    if(!valid_terminal(checkpoint, report, &error) || checkpoint.revision != expected_revision + 1)
        return {AssuranceStoreStatus::Invalid, 0, {},
                error.empty() ? "terminal checkpoint revision must advance exactly once" : error};
    const auto checkpoint_document = encode(checkpoint);
    const auto checkpoint_digest = checkpoint_document.at("canonical_digest").get<std::string>();
    const auto report_document = encode(report);
    const auto report_digest = report_document.at("canonical_digest").get<std::string>();
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    try {
        sqlite::Transaction transaction(db);
        sqlite::Statement insert(db, "INSERT INTO acceptance_reports(tenant_id,workflow_id,task_id,"
            "revision,report_json,report_digest) VALUES(?,?,?,?,?,?)");
        sqlite::bind_text(insert.get(), 1, checkpoint.metadata.identity.tenant_id);
        sqlite::bind_text(insert.get(), 2, checkpoint.workflow_id);
        sqlite::bind_text(insert.get(), 3, checkpoint.metadata.identity.task_id);
        sqlite3_bind_int64(insert.get(), 4, static_cast<sqlite3_int64>(checkpoint.revision));
        sqlite::bind_text(insert.get(), 5, report_document.dump());
        sqlite::bind_text(insert.get(), 6, report_digest);
        int rc = sqlite3_step(insert.get());
        if(rc == SQLITE_CONSTRAINT) return {AssuranceStoreStatus::AlreadyExists, 0, {}, {}};
        if(rc != SQLITE_DONE) return failure(db, rc);
        sqlite::Statement update(db, "UPDATE assurance_checkpoints SET revision=?,state=?,"
            "checkpoint_json=?,checkpoint_digest=?,updated_at=? WHERE tenant_id=? AND workflow_id=? AND revision=?");
        sqlite3_bind_int64(update.get(), 1, static_cast<sqlite3_int64>(checkpoint.revision));
        sqlite::bind_text(update.get(), 2, assurance_workflow_state_name(checkpoint.state));
        sqlite::bind_text(update.get(), 3, checkpoint_document.dump());
        sqlite::bind_text(update.get(), 4, checkpoint_digest);
        sqlite::bind_text(update.get(), 5, checkpoint.updated_at);
        sqlite::bind_text(update.get(), 6, checkpoint.metadata.identity.tenant_id);
        sqlite::bind_text(update.get(), 7, checkpoint.workflow_id);
        sqlite3_bind_int64(update.get(), 8, static_cast<sqlite3_int64>(expected_revision));
        rc = sqlite3_step(update.get());
        if(rc != SQLITE_DONE) return failure(db, rc);
        if(sqlite3_changes(db) != 1)
            return {AssuranceStoreStatus::RevisionConflict, 0, {}, "checkpoint CAS failed"};
        transaction.commit();
        return {AssuranceStoreStatus::Committed, checkpoint.revision, report_digest, {}};
    } catch(const std::exception& exception) {
        return {AssuranceStoreStatus::Error, 0, {}, exception.what()};
    }
}

std::optional<StoredAcceptanceReport> SQLiteAssuranceStore::load_report(
    std::string_view tenant_id, std::string_view workflow_id) {
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    sqlite::Statement statement(db, "SELECT revision,report_json,report_digest FROM "
                                    "acceptance_reports WHERE tenant_id=? AND workflow_id=?");
    sqlite::bind_text(statement.get(), 1, tenant_id);
    sqlite::bind_text(statement.get(), 2, workflow_id);
    const int rc = sqlite3_step(statement.get());
    if(rc == SQLITE_DONE) return std::nullopt;
    if(rc != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(db));
    const auto revision = static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 0));
    const auto document_text = sqlite::column_text(statement.get(), 1);
    const auto stored_digest = sqlite::column_text(statement.get(), 2);
    auto report = decode_acceptance_report(json::parse(document_text));
    if(!report || encode(*report).at("canonical_digest").get<std::string>() != stored_digest)
        throw std::runtime_error("stored acceptance report is corrupt");
    return StoredAcceptanceReport{std::move(*report), revision, std::string(workflow_id)};
}

}  // namespace agent_framework::assurance
