#include "agent/ui/store_backed_operations.hpp"

#include <filesystem>
#include <stdexcept>

#include <sqlite3.h>

#include "agent/contracts/contract.hpp"
#include "agent/harness/runtime.hpp"
#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework {
namespace {
namespace sqlite = internal::sqlite;
std::string snapshot_digest(const Phase4OperationsSnapshot& snapshot) {
    auto document = Phase4OperationsProjection::to_json(snapshot);
    document.erase("snapshot_id");
    return contracts::canonical_digest(document).value_or("sha256:unavailable");
}
}

SQLiteOperationsSnapshotStore::SQLiteOperationsSnapshotStore(std::string path) {
    if(path.empty()) throw std::invalid_argument("operations snapshot path is required");
    const std::filesystem::path file(path); std::error_code ec;
    if(file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    sqlite3* opened = nullptr;
    if(ec || sqlite3_open_v2(path.c_str(), &opened, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                            SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        const auto message = ec ? ec.message() : (opened ? sqlite3_errmsg(opened) : "sqlite open failed");
        if(opened) sqlite3_close(opened);
        throw std::runtime_error(message);
    }
    db_ = opened; sqlite3_busy_timeout(opened, 3000);
    sqlite::exec(opened, "PRAGMA journal_mode=WAL"); sqlite::exec(opened, "PRAGMA synchronous=FULL");
    sqlite::exec(opened, "CREATE TABLE IF NOT EXISTS phase4_operations_snapshots("
        "snapshot_id TEXT PRIMARY KEY,tenant_id TEXT NOT NULL,run_id TEXT NOT NULL,"
        "updated_at TEXT NOT NULL,document_json TEXT NOT NULL,document_digest TEXT NOT NULL)");
    sqlite::exec(opened, "CREATE INDEX IF NOT EXISTS phase4_operations_latest ON "
                         "phase4_operations_snapshots(tenant_id,run_id,updated_at)");
#if !defined(_WIN32)
    std::filesystem::permissions(file, std::filesystem::perms::owner_read |
        std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, ec);
    if(ec) throw std::runtime_error(ec.message());
#endif
}
SQLiteOperationsSnapshotStore::~SQLiteOperationsSnapshotStore() {
    if(db_) sqlite3_close(sqlite::database(db_));
}
bool SQLiteOperationsSnapshotStore::save(const Phase4OperationsSnapshot& snapshot, std::string* error) {
    std::lock_guard lock(mutex_); auto* db = sqlite::database(db_);
    try {
        const auto document = Phase4OperationsProjection::to_json(snapshot).dump();
        sqlite::Statement insert(db, "INSERT INTO phase4_operations_snapshots(snapshot_id,tenant_id,run_id,"
            "updated_at,document_json,document_digest) VALUES(?,?,?,?,?,?)");
        sqlite::bind_text(insert.get(), 1, snapshot.snapshot_id);
        sqlite::bind_text(insert.get(), 2, snapshot.tenant_id);
        sqlite::bind_text(insert.get(), 3, snapshot.run_id);
        sqlite::bind_text(insert.get(), 4, snapshot.updated_at);
        sqlite::bind_text(insert.get(), 5, document);
        sqlite::bind_text(insert.get(), 6, contracts::canonical_digest(nlohmann::json::parse(document)).value_or(""));
        const int rc = sqlite::step(insert.get());
        if(rc == SQLITE_CONSTRAINT) return true;
        if(rc != SQLITE_DONE) { if(error) *error = sqlite3_errmsg(db); return false; }
        return true;
    } catch(const std::exception& e) { if(error) *error = e.what(); return false; }
}
std::optional<Phase4OperationsSnapshot> SQLiteOperationsSnapshotStore::load(std::string_view id) {
    std::lock_guard lock(mutex_); auto* db = sqlite::database(db_);
    sqlite::Statement query(db, "SELECT document_json,document_digest FROM phase4_operations_snapshots WHERE snapshot_id=?");
    sqlite::bind_text(query.get(), 1, id);
    if(sqlite::step(query.get()) != SQLITE_ROW) return std::nullopt;
    const auto document = sqlite::column_text(query.get(), 0);
    const auto json = nlohmann::json::parse(document);
    if(contracts::canonical_digest(json).value_or("") != sqlite::column_text(query.get(), 1))
        throw std::runtime_error("operations snapshot digest mismatch");
    return Phase4OperationsProjection::from_json(json);
}
std::optional<Phase4OperationsSnapshot> SQLiteOperationsSnapshotStore::latest(
    std::string_view tenant, std::string_view run) {
    std::lock_guard lock(mutex_); auto* db = sqlite::database(db_);
    sqlite::Statement query(db, "SELECT document_json,document_digest FROM phase4_operations_snapshots "
                                "WHERE tenant_id=? AND run_id=? ORDER BY updated_at DESC,rowid DESC LIMIT 1");
    sqlite::bind_text(query.get(), 1, tenant); sqlite::bind_text(query.get(), 2, run);
    if(sqlite::step(query.get()) != SQLITE_ROW) return std::nullopt;
    const auto document = sqlite::column_text(query.get(), 0); const auto json = nlohmann::json::parse(document);
    if(contracts::canonical_digest(json).value_or("") != sqlite::column_text(query.get(), 1))
        throw std::runtime_error("operations snapshot digest mismatch");
    return Phase4OperationsProjection::from_json(json);
}

StoreBackedOperationsAssembler::StoreBackedOperationsAssembler(
    harness::HarnessStore& harnesses, approval::ApprovalStore& approvals,
    memory_v2::MemoryStore& memories, SQLiteOperationsSnapshotStore& snapshots)
    : harnesses_(harnesses), approvals_(approvals), memories_(memories), snapshots_(snapshots) {}

std::optional<Phase4OperationsSnapshot> StoreBackedOperationsAssembler::assemble(
    const OperationsAssemblyRequest& request, std::string* error) {
    auto stored = harnesses_.load(request.tenant_id, request.harness_id);
    if(!stored) { if(error) *error = "harness not found"; return std::nullopt; }
    auto out = harness::Phase4HarnessRuntime::project_operations(stored->checkpoint);
    out.updated_at = request.now;
    out.source_revisions.push_back({"harness", request.tenant_id + "/" + request.harness_id,
                                    stored->revision, stored->digest});
    const auto pending = approvals_.pending(request.tenant_id, request.now, 100);
    for(const auto& item : pending) {
        const auto digest = approval::encode(item).at("canonical_digest").get<std::string>();
        out.hitl.push_back({item.approval_id, item.request_kind, OperationsStatus::Pending,
            "Accountable review required for " + item.scope, item.requester_id,
            item.expires_at, {"approve", "request_remediation", "reject"}});
        out.source_revisions.push_back({"approval", request.tenant_id + "/" + item.approval_id, 0, digest});
    }
    memory_v2::MemoryQuery query;
    query.subject = request.memory_subject; query.principal_id = request.principal_id; query.limit = 100;
    for(const auto& item : memories_.query(query)) {
        const auto digest = memory_v2::encode(item).at("canonical_digest").get<std::string>();
        out.memory.push_back({item.record_id, std::to_string(static_cast<int>(item.scope.level)),
            item.source_kind, std::to_string(static_cast<int>(item.authority)), item.freshness_deadline,
            item.conflicts_with.empty() ? "none" : "conflict", false, "available to active view"});
        out.source_revisions.push_back({"memory", request.tenant_id + "/" + item.record_id,
                                        item.revision, digest});
    }
    out.snapshot_id = snapshot_digest(out);
    if(!snapshots_.save(out, error)) return std::nullopt;
    return out;
}
}  // namespace agent_framework
