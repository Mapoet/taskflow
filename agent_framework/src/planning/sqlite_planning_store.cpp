#include "agent/planning/plan_store.hpp"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

#include <sqlite3.h>

#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::planning {
namespace {
using json = nlohmann::json;
namespace sqlite = internal::sqlite;

PlanningCommitResult sqlite_failure(sqlite3* db, int status) {
    if(status == SQLITE_BUSY || status == SQLITE_LOCKED)
        return {PlanningCommitStatus::Busy, {}, "planning database is busy"};
    if(status == SQLITE_CONSTRAINT)
        return {PlanningCommitStatus::Duplicate, {}, sqlite3_errmsg(db)};
    return {PlanningCommitStatus::Error, {}, sqlite3_errmsg(db)};
}

bool valid_evidence(const contracts::ContractMetadata& scope, const EvidenceRecord& record,
                    std::string* error) {
    if(scope.identity.tenant_id.empty() || scope.identity.task_id.empty() ||
       record.evidence_id.empty() || record.content_digest.empty() || record.locator.empty()) {
        if(error) *error = "scope, id, locator, and digest are required";
        return false;
    }
    if(record.instruction_authority &&
       (record.origin_kind == "external" || record.origin_kind == "rag" ||
        record.origin_kind == "web" || record.origin_kind == "tool")) {
        if(error) *error = "untrusted evidence cannot carry instruction authority";
        return false;
    }
    return true;
}

std::string evidence_document(const contracts::ContractMetadata& scope,
                              const EvidenceRecord& record) {
    EvidenceBundle bundle;
    bundle.metadata = scope;
    bundle.bundle_id = record.content_digest;
    bundle.records = {record};
    return contracts::canonical_json(encode(bundle));
}

std::optional<EvidenceRecord> decode_evidence(std::string_view document) {
    try {
        const auto bundle = decode_evidence_bundle(json::parse(document));
        if(!bundle || bundle->records.size() != 1) return std::nullopt;
        return bundle->records.front();
    } catch(...) {
        return std::nullopt;
    }
}

std::optional<ExecutionPlan> decode_plan(std::string_view document) {
    try { return decode_execution_plan(json::parse(document)); }
    catch(...) { return std::nullopt; }
}

bool valid_initial_plan(const ExecutionPlan& plan) {
    return !plan.metadata.identity.tenant_id.empty() &&
           !plan.metadata.identity.task_id.empty() &&
           !plan.metadata.identity.plan_id.empty() &&
           plan.plan_revision == 1 && plan.parent_plan_digest.empty();
}
}  // namespace

SQLitePlanningStore::SQLitePlanningStore(std::string path, SQLitePlanningStoreOptions options)
    : path_(std::move(path)), options_(options) {
    if(path_.empty()) throw std::invalid_argument("planning store path must not be empty");
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

SQLitePlanningStore::~SQLitePlanningStore() {
    if(db_) sqlite3_close(sqlite::database(db_));
}

void SQLitePlanningStore::migrate() {
    auto* db = sqlite::database(db_);
    sqlite::exec(db, "BEGIN IMMEDIATE");
    try {
        sqlite::exec(db, "CREATE TABLE IF NOT EXISTS planning_schema_version("
                    "version INTEGER PRIMARY KEY,applied_at TEXT NOT NULL)");
        int version = 0;
        {
            sqlite::Statement query(db, "SELECT COALESCE(MAX(version),0) FROM planning_schema_version");
            if(sqlite::step(query.get()) == SQLITE_ROW)
                version = sqlite::column_int(query.get(), 0);
        }
        if(version > 1) throw std::runtime_error("planning schema newer than binary");
        if(version == 0) {
            sqlite::exec(db, "CREATE TABLE planning_evidence("
                        "tenant_id TEXT NOT NULL,task_id TEXT NOT NULL,evidence_id TEXT NOT NULL,"
                        "locator TEXT NOT NULL,content_digest TEXT NOT NULL,document_json TEXT NOT NULL,"
                        "created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%fZ','now')),"
                        "PRIMARY KEY(tenant_id,task_id,evidence_id),"
                        "UNIQUE(tenant_id,task_id,locator,content_digest))");
            sqlite::exec(db, "CREATE TABLE planning_plans("
                        "tenant_id TEXT NOT NULL,task_id TEXT NOT NULL,plan_id TEXT NOT NULL,"
                        "revision INTEGER NOT NULL,plan_digest TEXT NOT NULL,parent_digest TEXT NOT NULL,"
                        "document_json TEXT NOT NULL,"
                        "created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%fZ','now')),"
                        "PRIMARY KEY(tenant_id,task_id,plan_id,revision),"
                        "UNIQUE(tenant_id,task_id,plan_id,plan_digest))");
            sqlite::exec(db, "INSERT INTO planning_schema_version VALUES("
                        "1,strftime('%Y-%m-%dT%H:%M:%fZ','now'))");
        }
        sqlite::exec(db, "COMMIT");
    } catch(...) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

PlanningCommitResult SQLitePlanningStore::append(
    const contracts::ContractMetadata& scope, const EvidenceRecord& record) {
    std::string validation_error;
    if(!valid_evidence(scope, record, &validation_error))
        return {PlanningCommitStatus::Invalid, {}, std::move(validation_error)};
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    sqlite::Statement statement(db, "INSERT INTO planning_evidence(tenant_id,task_id,evidence_id,locator,"
                            "content_digest,document_json) VALUES(?,?,?,?,?,?)");
    sqlite::bind_text(statement.get(), 1, scope.identity.tenant_id);
    sqlite::bind_text(statement.get(), 2, scope.identity.task_id);
    sqlite::bind_text(statement.get(), 3, record.evidence_id);
    sqlite::bind_text(statement.get(), 4, record.locator);
    sqlite::bind_text(statement.get(), 5, record.content_digest);
    sqlite::bind_text(statement.get(), 6, evidence_document(scope, record));
    const auto status = sqlite::step(statement.get());
    if(status != SQLITE_DONE) {
        auto result = sqlite_failure(db, status);
        result.digest = record.content_digest;
        return result;
    }
    return {PlanningCommitStatus::Committed, record.content_digest, {}};
}

std::optional<EvidenceRecord> SQLitePlanningStore::get(
    const contracts::ContractMetadata& scope, std::string_view evidence_id) {
    std::lock_guard lock(mutex_);
    sqlite::Statement query(sqlite::database(db_), "SELECT document_json FROM planning_evidence "
                                   "WHERE tenant_id=? AND task_id=? AND evidence_id=?");
    sqlite::bind_text(query.get(), 1, scope.identity.tenant_id);
    sqlite::bind_text(query.get(), 2, scope.identity.task_id);
    sqlite::bind_text(query.get(), 3, evidence_id);
    return sqlite::step(query.get()) == SQLITE_ROW
        ? decode_evidence(sqlite::column_text(query.get(), 0)) : std::nullopt;
}

EvidenceBundle SQLitePlanningStore::bundle(
    const contracts::ContractMetadata& scope, const std::vector<std::string>& evidence_ids) {
    EvidenceBundle result;
    result.metadata = scope;
    std::lock_guard lock(mutex_);
    for(const auto& id : evidence_ids) {
        sqlite::Statement query(sqlite::database(db_), "SELECT document_json FROM planning_evidence "
                                       "WHERE tenant_id=? AND task_id=? AND evidence_id=?");
        sqlite::bind_text(query.get(), 1, scope.identity.tenant_id);
        sqlite::bind_text(query.get(), 2, scope.identity.task_id);
        sqlite::bind_text(query.get(), 3, id);
        if(sqlite::step(query.get()) == SQLITE_ROW) {
            auto record = decode_evidence(sqlite::column_text(query.get(), 0));
            if(record) result.records.push_back(std::move(*record));
        }
    }
    std::sort(result.records.begin(), result.records.end(),
              [](const auto& left, const auto& right) {
                  return left.evidence_id < right.evidence_id;
              });
    json basis = json::array();
    for(const auto& record : result.records)
        basis.push_back({record.evidence_id, record.content_digest});
    result.bundle_id = contracts::embedded_digest(basis).value_or("");
    return result;
}

PlanningCommitResult SQLitePlanningStore::create(const ExecutionPlan& plan) {
    if(!valid_initial_plan(plan))
        return {PlanningCommitStatus::Invalid, {}, "initial plan identity/revision is invalid"};
    const auto document = encode(plan);
    const auto digest = document.at("canonical_digest").get<std::string>();
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    sqlite::Statement statement(db, "INSERT INTO planning_plans(tenant_id,task_id,plan_id,revision,"
                            "plan_digest,parent_digest,document_json) VALUES(?,?,?,?,?,?,?)");
    sqlite::bind_text(statement.get(), 1, plan.metadata.identity.tenant_id);
    sqlite::bind_text(statement.get(), 2, plan.metadata.identity.task_id);
    sqlite::bind_text(statement.get(), 3, plan.metadata.identity.plan_id);
    sqlite::bind_int64(statement.get(), 4, static_cast<sqlite3_int64>(plan.plan_revision));
    sqlite::bind_text(statement.get(), 5, digest);
    sqlite::bind_text(statement.get(), 6, plan.parent_plan_digest);
    sqlite::bind_text(statement.get(), 7, contracts::canonical_json(document));
    const auto status = sqlite::step(statement.get());
    if(status != SQLITE_DONE) {
        auto result = sqlite_failure(db, status);
        result.digest = digest;
        return result;
    }
    return {PlanningCommitStatus::Committed, digest, {}};
}

PlanningCommitResult SQLitePlanningStore::compare_exchange(
    const ExecutionPlan& plan, std::uint64_t expected_revision) {
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    sqlite::exec(db, "BEGIN IMMEDIATE");
    try {
        std::uint64_t current_revision = 0;
        std::string current_digest;
        {
            sqlite::Statement query(db, "SELECT revision,plan_digest FROM planning_plans "
                                "WHERE tenant_id=? AND task_id=? AND plan_id=? "
                                "ORDER BY revision DESC LIMIT 1");
            sqlite::bind_text(query.get(), 1, plan.metadata.identity.tenant_id);
            sqlite::bind_text(query.get(), 2, plan.metadata.identity.task_id);
            sqlite::bind_text(query.get(), 3, plan.metadata.identity.plan_id);
            if(sqlite::step(query.get()) != SQLITE_ROW) {
                sqlite::exec(db, "ROLLBACK");
                return {PlanningCommitStatus::NotFound, {}, "plan not found"};
            }
            current_revision = static_cast<std::uint64_t>(sqlite::column_int64(query.get(), 0));
            current_digest = sqlite::column_text(query.get(), 1);
        }
        if(current_revision != expected_revision) {
            sqlite::exec(db, "ROLLBACK");
            return {PlanningCommitStatus::RevisionConflict, current_digest,
                    "plan revision conflict"};
        }
        if(plan.plan_revision != expected_revision + 1 ||
           plan.parent_plan_digest != current_digest) {
            sqlite::exec(db, "ROLLBACK");
            return {PlanningCommitStatus::Invalid, {},
                    "new revision must bind the current parent digest"};
        }
        const auto document = encode(plan);
        const auto digest = document.at("canonical_digest").get<std::string>();
        sqlite::Statement insert(db, "INSERT INTO planning_plans(tenant_id,task_id,plan_id,revision,"
                             "plan_digest,parent_digest,document_json) VALUES(?,?,?,?,?,?,?)");
        sqlite::bind_text(insert.get(), 1, plan.metadata.identity.tenant_id);
        sqlite::bind_text(insert.get(), 2, plan.metadata.identity.task_id);
        sqlite::bind_text(insert.get(), 3, plan.metadata.identity.plan_id);
        sqlite::bind_int64(insert.get(), 4, static_cast<sqlite3_int64>(plan.plan_revision));
        sqlite::bind_text(insert.get(), 5, digest);
        sqlite::bind_text(insert.get(), 6, plan.parent_plan_digest);
        sqlite::bind_text(insert.get(), 7, contracts::canonical_json(document));
        const auto status = sqlite::step(insert.get());
        if(status != SQLITE_DONE) {
            auto result = sqlite_failure(db, status);
            sqlite::exec(db, "ROLLBACK");
            return result;
        }
        sqlite::exec(db, "COMMIT");
        return {PlanningCommitStatus::Committed, digest, {}};
    } catch(...) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

std::optional<ExecutionPlan> SQLitePlanningStore::current(
    const contracts::ContractIdentity& identity) {
    std::lock_guard lock(mutex_);
    sqlite::Statement query(sqlite::database(db_), "SELECT document_json FROM planning_plans "
                                   "WHERE tenant_id=? AND task_id=? AND plan_id=? "
                                   "ORDER BY revision DESC LIMIT 1");
    sqlite::bind_text(query.get(), 1, identity.tenant_id);
    sqlite::bind_text(query.get(), 2, identity.task_id);
    sqlite::bind_text(query.get(), 3, identity.plan_id);
    return sqlite::step(query.get()) == SQLITE_ROW
        ? decode_plan(sqlite::column_text(query.get(), 0)) : std::nullopt;
}

std::vector<ExecutionPlan> SQLitePlanningStore::history(
    const contracts::ContractIdentity& identity) {
    std::vector<ExecutionPlan> result;
    std::lock_guard lock(mutex_);
    sqlite::Statement query(sqlite::database(db_), "SELECT document_json FROM planning_plans "
                                   "WHERE tenant_id=? AND task_id=? AND plan_id=? "
                                   "ORDER BY revision ASC");
    sqlite::bind_text(query.get(), 1, identity.tenant_id);
    sqlite::bind_text(query.get(), 2, identity.task_id);
    sqlite::bind_text(query.get(), 3, identity.plan_id);
    while(sqlite::step(query.get()) == SQLITE_ROW) {
        auto plan = decode_plan(sqlite::column_text(query.get(), 0));
        if(plan) result.push_back(std::move(*plan));
    }
    return result;
}

}  // namespace agent_framework::planning
