#include "agent/ui/store_backed_operations.hpp"

#include <filesystem>
#include <stdexcept>
#include <unordered_set>
#include <vector>

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

Phase4OperationsSnapshot decode_snapshot(std::string_view document,
                                         std::string_view expected_digest,
                                         bool* recovered = nullptr) {
    auto root = nlohmann::json::parse(document);
    if(contracts::canonical_digest(root).value_or("") != expected_digest)
        throw std::runtime_error("operations snapshot digest mismatch");
    bool changed = false;
    if(root.contains("invocations") && root["invocations"].is_array() &&
       root["invocations"].size() > Phase4OperationsProjection::max_items) {
        const auto& input = root["invocations"];
        std::size_t non_invocation_sources = 0;
        if(root.contains("source_revisions") && root["source_revisions"].is_array()) {
            for(const auto& source : root["source_revisions"])
                if(!source.is_object() || source.value("store", "") != "tool_invocation_events")
                    ++non_invocation_sources;
        }
        const auto capacity = non_invocation_sources >= Phase4OperationsProjection::max_items
            ? std::size_t{0} : Phase4OperationsProjection::max_items - non_invocation_sources;
        std::vector<bool> keep(input.size(), false);
        std::size_t kept = 0;
        const auto active = [](const nlohmann::json& value) {
            const auto status = value.is_object() ? value.value("status", "") : "";
            return status == "pending" || status == "running" || status == "blocked";
        };
        for(std::size_t i = 0; i < input.size(); ++i)
            if(active(input[i])) { keep[i] = true; ++kept; }
        if(kept > capacity)
            throw std::runtime_error("active operations invocations exceed bounded capacity");
        for(std::size_t i = input.size(); i > 0 && kept < capacity; --i)
            if(!keep[i - 1]) { keep[i - 1] = true; ++kept; }
        nlohmann::json retained = nlohmann::json::array();
        std::unordered_set<std::string> retained_ids;
        for(std::size_t i = 0; i < input.size(); ++i) if(keep[i]) {
            retained.push_back(input[i]);
            if(input[i].is_object()) retained_ids.insert(input[i].value("id", ""));
        }
        const auto removed = input.size() - retained.size();
        root["invocations"] = std::move(retained);
        root["invocations_compacted"] = root.value("invocations_compacted", std::uint64_t{0}) + removed;
        if(root.contains("source_revisions") && root["source_revisions"].is_array()) {
            nlohmann::json sources = nlohmann::json::array();
            for(const auto& source : root["source_revisions"]) {
                if(!source.is_object() || source.value("store", "") != "tool_invocation_events" ||
                   retained_ids.count(source.value("object_id", ""))) sources.push_back(source);
            }
            root["source_revisions"] = std::move(sources);
        }
        root["snapshot_id"] = "legacy-recovery-pending";
        changed = true;
    }
    auto snapshot = Phase4OperationsProjection::from_json(root);
    if(changed) snapshot.snapshot_id = snapshot_digest(snapshot);
    if(recovered) *recovered = changed;
    return snapshot;
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
        auto bounded = snapshot;
        if(Phase4OperationsProjection::compact_invocations(bounded) != 0)
            bounded.snapshot_id = snapshot_digest(bounded);
        const auto document = Phase4OperationsProjection::to_json(bounded).dump();
        sqlite::Statement insert(db, "INSERT INTO phase4_operations_snapshots(snapshot_id,tenant_id,run_id,"
            "updated_at,document_json,document_digest) VALUES(?,?,?,?,?,?)");
        sqlite::bind_text(insert.get(), 1, bounded.snapshot_id);
        sqlite::bind_text(insert.get(), 2, bounded.tenant_id);
        sqlite::bind_text(insert.get(), 3, bounded.run_id);
        sqlite::bind_text(insert.get(), 4, bounded.updated_at);
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
    return decode_snapshot(document, sqlite::column_text(query.get(), 1));
}
std::optional<Phase4OperationsSnapshot> SQLiteOperationsSnapshotStore::latest(
    std::string_view tenant, std::string_view run) {
    std::lock_guard lock(mutex_); auto* db = sqlite::database(db_);
    sqlite::Statement query(db, "SELECT document_json,document_digest FROM phase4_operations_snapshots "
                                "WHERE tenant_id=? AND run_id=? ORDER BY updated_at DESC,rowid DESC LIMIT 1");
    sqlite::bind_text(query.get(), 1, tenant); sqlite::bind_text(query.get(), 2, run);
    if(sqlite::step(query.get()) != SQLITE_ROW) return std::nullopt;
    const auto document = sqlite::column_text(query.get(), 0);
    bool recovered = false;
    auto snapshot = decode_snapshot(document, sqlite::column_text(query.get(), 1), &recovered);
    if(recovered) {
        const auto recovered_document = Phase4OperationsProjection::to_json(snapshot).dump();
        sqlite::Statement insert(db, "INSERT OR IGNORE INTO phase4_operations_snapshots(snapshot_id,tenant_id,run_id,"
            "updated_at,document_json,document_digest) VALUES(?,?,?,?,?,?)");
        sqlite::bind_text(insert.get(), 1, snapshot.snapshot_id);
        sqlite::bind_text(insert.get(), 2, snapshot.tenant_id);
        sqlite::bind_text(insert.get(), 3, snapshot.run_id);
        sqlite::bind_text(insert.get(), 4, snapshot.updated_at);
        sqlite::bind_text(insert.get(), 5, recovered_document);
        sqlite::bind_text(insert.get(), 6, contracts::canonical_digest(
            nlohmann::json::parse(recovered_document)).value_or(""));
        if(sqlite::step(insert.get()) != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    return snapshot;
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
