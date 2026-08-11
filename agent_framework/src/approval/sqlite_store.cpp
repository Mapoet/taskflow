#include "agent/approval/store.hpp"

#include <filesystem>
#include <stdexcept>

#include <sqlite3.h>

#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::approval {
namespace {
namespace sqlite = internal::sqlite;
std::optional<ApprovalRequest> decode_request(std::string_view document) {
    try { return decode_approval_request(nlohmann::json::parse(document)); }
    catch(...) { return std::nullopt; }
}
std::optional<ApprovalDecision> decode_decision(std::string_view document) {
    try { return decode_approval_decision(nlohmann::json::parse(document)); }
    catch(...) { return std::nullopt; }
}
ApprovalStoreResult failure(sqlite3* db, int status) {
    if(status == SQLITE_BUSY || status == SQLITE_LOCKED)
        return {ApprovalStoreStatus::Busy, 0, {}, sqlite3_errmsg(db)};
    if(status == SQLITE_CONSTRAINT)
        return {ApprovalStoreStatus::Duplicate, 0, {}, sqlite3_errmsg(db)};
    return {ApprovalStoreStatus::Error, 0, {}, sqlite3_errmsg(db)};
}
bool bindings_match(const ApprovalRequest& request, const ApprovalDecision& decision) {
    return request.metadata.identity.tenant_id == decision.metadata.identity.tenant_id &&
           request.metadata.identity.task_id == decision.metadata.identity.task_id &&
           request.approval_id == decision.approval_id &&
           request.policy_revision == decision.policy_revision &&
           request.plan_digest == decision.plan_digest &&
           request.arguments_digest == decision.arguments_digest &&
           request.artifact_digest == decision.artifact_digest &&
           request.memory_view_digest == decision.memory_view_digest;
}
}  // namespace

SQLiteApprovalStore::SQLiteApprovalStore(std::string path, SQLiteApprovalStoreOptions options)
    : path_(std::move(path)), options_(options) {
    if(path_.empty()) throw std::invalid_argument("approval store path must not be empty");
    const std::filesystem::path file(path_);
    std::error_code error;
    if(file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), error);
    if(error) throw std::runtime_error(error.message());
    sqlite3* opened = nullptr;
    const auto status = sqlite3_open_v2(path_.c_str(), &opened,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if(status != SQLITE_OK) {
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

SQLiteApprovalStore::~SQLiteApprovalStore() {
    if(db_) sqlite3_close(sqlite::database(db_));
}

void SQLiteApprovalStore::migrate() {
    auto* db = sqlite::database(db_);
    sqlite::exec(db, "BEGIN IMMEDIATE");
    try {
        sqlite::exec(db, "CREATE TABLE IF NOT EXISTS approval_schema_version("
                    "version INTEGER PRIMARY KEY,applied_at TEXT NOT NULL)");
        int version = 0;
        {
            sqlite::Statement query(db, "SELECT COALESCE(MAX(version),0) FROM approval_schema_version");
            if(sqlite::step(query.get()) == SQLITE_ROW)
                version = sqlite::column_int(query.get(), 0);
        }
        if(version > 1) throw std::runtime_error("approval schema newer than binary");
        if(version == 0) {
            sqlite::exec(db, "CREATE TABLE approval_requests("
                        "approval_id TEXT PRIMARY KEY,tenant_id TEXT NOT NULL,task_id TEXT NOT NULL,"
                        "request_digest TEXT NOT NULL UNIQUE,expires_at TEXT NOT NULL,"
                        "document_json TEXT NOT NULL,"
                        "created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%fZ','now')))");
            sqlite::exec(db, "CREATE INDEX approval_pending_idx ON approval_requests(tenant_id,expires_at)");
            sqlite::exec(db, "CREATE TABLE approval_decisions("
                        "approval_id TEXT NOT NULL,revision INTEGER NOT NULL,decision INTEGER NOT NULL,"
                        "document_json TEXT NOT NULL,"
                        "created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%fZ','now')),"
                        "PRIMARY KEY(approval_id,revision),"
                        "FOREIGN KEY(approval_id) REFERENCES approval_requests(approval_id))");
            sqlite::exec(db, "INSERT INTO approval_schema_version VALUES("
                        "1,strftime('%Y-%m-%dT%H:%M:%fZ','now'))");
        }
        sqlite::exec(db, "COMMIT");
    } catch(...) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

ApprovalStoreResult SQLiteApprovalStore::put_request(const ApprovalRequest& request_value) {
    if(request_value.approval_id.empty() || request_value.metadata.identity.tenant_id.empty() ||
       request_value.metadata.identity.task_id.empty() || request_value.requester_id.empty() ||
       request_value.policy_revision.empty())
        return {ApprovalStoreStatus::Invalid, 0, {}, "approval identity and policy are required"};
    const auto document = encode(request_value);
    const auto digest = document.at("canonical_digest").get<std::string>();
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    sqlite::Statement insert(db, "INSERT INTO approval_requests(approval_id,tenant_id,task_id,"
                         "request_digest,expires_at,document_json) VALUES(?,?,?,?,?,?)");
    sqlite::bind_text(insert.get(), 1, request_value.approval_id);
    sqlite::bind_text(insert.get(), 2, request_value.metadata.identity.tenant_id);
    sqlite::bind_text(insert.get(), 3, request_value.metadata.identity.task_id);
    sqlite::bind_text(insert.get(), 4, digest);
    sqlite::bind_text(insert.get(), 5, request_value.expires_at);
    sqlite::bind_text(insert.get(), 6, contracts::canonical_json(document));
    const auto status = sqlite::step(insert.get());
    if(status != SQLITE_DONE) {
        auto result = failure(db, status);
        result.digest = digest;
        return result;
    }
    return {ApprovalStoreStatus::Committed, 0, digest, {}};
}

std::optional<ApprovalRequest> SQLiteApprovalStore::request(std::string_view approval_id) {
    std::lock_guard lock(mutex_);
    sqlite::Statement query(sqlite::database(db_), "SELECT document_json FROM approval_requests WHERE approval_id=?");
    sqlite::bind_text(query.get(), 1, approval_id);
    return sqlite::step(query.get()) == SQLITE_ROW
        ? decode_request(sqlite::column_text(query.get(), 0)) : std::nullopt;
}

ApprovalStoreResult SQLiteApprovalStore::decide(
    const ApprovalDecision& decision_value, std::uint64_t expected_revision) {
    if(decision_value.approval_id.empty() || decision_value.reviewer_id.empty() ||
       decision_value.request_digest.empty())
        return {ApprovalStoreStatus::Invalid, 0, {}, "decision identity is required"};
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    sqlite::exec(db, "BEGIN IMMEDIATE");
    try {
        ApprovalRequest request_value;
        std::string request_digest;
        {
            sqlite::Statement query(db, "SELECT request_digest,document_json FROM approval_requests "
                                "WHERE approval_id=?");
            sqlite::bind_text(query.get(), 1, decision_value.approval_id);
            if(sqlite::step(query.get()) != SQLITE_ROW) {
                sqlite::exec(db, "ROLLBACK");
                return {ApprovalStoreStatus::NotFound, 0, {}, "approval request not found"};
            }
            request_digest = sqlite::column_text(query.get(), 0);
            const auto decoded = decode_request(sqlite::column_text(query.get(), 1));
            if(!decoded) throw std::runtime_error("stored approval request is invalid");
            request_value = *decoded;
        }
        std::uint64_t current_revision = 0;
        int current_decision = -1;
        {
            sqlite::Statement query(db, "SELECT revision,decision FROM approval_decisions "
                                "WHERE approval_id=? ORDER BY revision DESC LIMIT 1");
            sqlite::bind_text(query.get(), 1, decision_value.approval_id);
            if(sqlite::step(query.get()) == SQLITE_ROW) {
                current_revision = static_cast<std::uint64_t>(sqlite::column_int64(query.get(), 0));
                current_decision = sqlite::column_int(query.get(), 1);
            }
        }
        if(current_revision != expected_revision) {
            sqlite::exec(db, "ROLLBACK");
            return {ApprovalStoreStatus::RevisionConflict, current_revision, {},
                    "approval decision revision conflict"};
        }
        if(decision_value.request_digest != request_digest ||
           !bindings_match(request_value, decision_value) ||
           decision_value.reviewer_id == request_value.requester_id) {
            sqlite::exec(db, "ROLLBACK");
            return {ApprovalStoreStatus::Invalid, current_revision, {},
                    "decision violates request digest, bindings, or separation of duties"};
        }
        if(!request_value.expires_at.empty() && !decision_value.decided_at.empty() &&
           decision_value.decided_at > request_value.expires_at &&
           decision_value.decision != Decision::Expired) {
            sqlite::exec(db, "ROLLBACK");
            return {ApprovalStoreStatus::Invalid, current_revision, {},
                    "expired request cannot be approved"};
        }
        if(current_revision != 0 &&
           !(static_cast<Decision>(current_decision) == Decision::Approved &&
             decision_value.decision == Decision::Revoked)) {
            sqlite::exec(db, "ROLLBACK");
            return {ApprovalStoreStatus::Invalid, current_revision, {},
                    "terminal decision only permits approved to revoked"};
        }
        const auto revision = current_revision + 1;
        const auto document = encode(decision_value);
        const auto digest = document.at("canonical_digest").get<std::string>();
        sqlite::Statement insert(db, "INSERT INTO approval_decisions(approval_id,revision,decision,"
                             "document_json) VALUES(?,?,?,?)");
        sqlite::bind_text(insert.get(), 1, decision_value.approval_id);
        sqlite::bind_int64(insert.get(), 2, static_cast<sqlite3_int64>(revision));
        sqlite::bind_int(insert.get(), 3, static_cast<int>(decision_value.decision));
        sqlite::bind_text(insert.get(), 4, contracts::canonical_json(document));
        const auto status = sqlite::step(insert.get());
        if(status != SQLITE_DONE) {
            auto result = failure(db, status);
            sqlite::exec(db, "ROLLBACK");
            return result;
        }
        sqlite::exec(db, "COMMIT");
        return {ApprovalStoreStatus::Committed, revision, digest, {}};
    } catch(...) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

std::optional<ApprovalDecision> SQLiteApprovalStore::latest_decision(
    std::string_view approval_id) {
    std::lock_guard lock(mutex_);
    sqlite::Statement query(sqlite::database(db_), "SELECT document_json FROM approval_decisions "
                                   "WHERE approval_id=? ORDER BY revision DESC LIMIT 1");
    sqlite::bind_text(query.get(), 1, approval_id);
    return sqlite::step(query.get()) == SQLITE_ROW
        ? decode_decision(sqlite::column_text(query.get(), 0)) : std::nullopt;
}

std::vector<ApprovalDecision> SQLiteApprovalStore::decision_history(
    std::string_view approval_id) {
    std::vector<ApprovalDecision> result;
    std::lock_guard lock(mutex_);
    sqlite::Statement query(sqlite::database(db_), "SELECT document_json FROM approval_decisions "
                                   "WHERE approval_id=? ORDER BY revision ASC");
    sqlite::bind_text(query.get(), 1, approval_id);
    while(sqlite::step(query.get()) == SQLITE_ROW) {
        auto decoded = decode_decision(sqlite::column_text(query.get(), 0));
        if(decoded) result.push_back(std::move(*decoded));
    }
    return result;
}

std::vector<ApprovalRequest> SQLiteApprovalStore::pending(
    std::string_view tenant_id, std::string_view now, std::size_t limit) {
    std::vector<ApprovalRequest> result;
    if(tenant_id.empty() || limit == 0) return result;
    std::lock_guard lock(mutex_);
    sqlite::Statement query(sqlite::database(db_), "SELECT r.document_json FROM approval_requests r "
        "WHERE r.tenant_id=? AND (r.expires_at='' OR r.expires_at>=?) "
        "AND NOT EXISTS(SELECT 1 FROM approval_decisions d WHERE d.approval_id=r.approval_id) "
        "ORDER BY r.created_at,r.approval_id LIMIT ?");
    sqlite::bind_text(query.get(), 1, tenant_id);
    sqlite::bind_text(query.get(), 2, now);
    sqlite::bind_int64(query.get(), 3, static_cast<sqlite3_int64>(limit));
    while(sqlite::step(query.get()) == SQLITE_ROW) {
        auto decoded = decode_request(sqlite::column_text(query.get(), 0));
        if(decoded) result.push_back(std::move(*decoded));
    }
    return result;
}

}  // namespace agent_framework::approval
