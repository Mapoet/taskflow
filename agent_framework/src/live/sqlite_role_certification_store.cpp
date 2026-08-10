#include "agent/live/role_certification.hpp"

#include <filesystem>
#include <stdexcept>

#include <sqlite3.h>

#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::live
{
    namespace
    {
        namespace sqlite = internal::sqlite;

        bool terminal(RoleCertificationState s) { return s == RoleCertificationState::Inconclusive || s == RoleCertificationState::AwaitingApproval || s == RoleCertificationState::Certified || s == RoleCertificationState::Rejected || s == RoleCertificationState::Failed || s == RoleCertificationState::Cancelled; }
        bool valid(const RoleCertificationCheckpoint &v) { return !v.metadata.identity.tenant_id.empty() && !v.metadata.identity.task_id.empty() && !v.workflow_id.empty() && v.revision > 0 && !v.environment_digest.empty() && !v.matrix_digest.empty(); }
        bool immutable(const RoleCertificationCheckpoint &a, const RoleCertificationCheckpoint &b) { return a.metadata.identity.tenant_id == b.metadata.identity.tenant_id && a.metadata.identity.task_id == b.metadata.identity.task_id && a.workflow_id == b.workflow_id && a.environment_digest == b.environment_digest && a.matrix_digest == b.matrix_digest; }
        bool report_match(const RoleCertificationCheckpoint &c, const RoleCertificationReport &r) { return terminal(c.state) && r.workflow_id == c.workflow_id && r.metadata.identity.tenant_id == c.metadata.identity.tenant_id && r.metadata.identity.task_id == c.metadata.identity.task_id && r.environment_digest == c.environment_digest && r.matrix_digest == c.matrix_digest && r.state == c.state && c.report_digest == encode(r).at("canonical_digest").get<std::string>(); }
        RoleCertificationStoreCommit fail(sqlite3 *db, int rc) { return {rc == SQLITE_BUSY || rc == SQLITE_LOCKED ? RoleCertificationStoreStatus::Busy : RoleCertificationStoreStatus::Error, 0, {}, sqlite3_errmsg(db)}; }
    }

    std::string InMemoryRoleCertificationStore::key(std::string_view t, std::string_view w) { return std::string(t) + '\n' + std::string(w); }
    RoleCertificationStoreCommit InMemoryRoleCertificationStore::create(const RoleCertificationCheckpoint &v)
    {
        if (!valid(v) || v.revision != 1)
            return {RoleCertificationStoreStatus::Invalid, 0, {}, "invalid initial checkpoint"};
        std::lock_guard lock(mutex_);
        auto k = key(v.metadata.identity.tenant_id, v.workflow_id);
        if (checkpoints_.count(k))
            return {RoleCertificationStoreStatus::AlreadyExists, 0, {}, {}};
        checkpoints_[k] = {v, v.revision};
        return {RoleCertificationStoreStatus::Committed, v.revision, encode(v).at("canonical_digest"), {}};
    }
    std::optional<StoredRoleCertificationCheckpoint> InMemoryRoleCertificationStore::load(std::string_view t, std::string_view w)
    {
        std::lock_guard lock(mutex_);
        auto it = checkpoints_.find(key(t, w));
        return it == checkpoints_.end() ? std::nullopt : std::optional(it->second);
    }
    RoleCertificationStoreCommit InMemoryRoleCertificationStore::compare_exchange(const RoleCertificationCheckpoint &v, std::uint64_t expected)
    {
        if (!valid(v) || v.revision != expected + 1)
            return {RoleCertificationStoreStatus::Invalid, 0, {}, "revision must advance once"};
        std::lock_guard lock(mutex_);
        auto it = checkpoints_.find(key(v.metadata.identity.tenant_id, v.workflow_id));
        if (it == checkpoints_.end())
            return {RoleCertificationStoreStatus::NotFound, 0, {}, {}};
        if (it->second.revision != expected)
            return {RoleCertificationStoreStatus::RevisionConflict, it->second.revision, {}, {}};
        if (!immutable(it->second.checkpoint, v))
            return {RoleCertificationStoreStatus::Invalid, expected, {}, "immutable bindings changed"};
        it->second = {v, v.revision};
        return {RoleCertificationStoreStatus::Committed, v.revision, encode(v).at("canonical_digest"), {}};
    }
    RoleCertificationStoreCommit InMemoryRoleCertificationStore::commit_report(const RoleCertificationCheckpoint &v, std::uint64_t expected, const RoleCertificationReport &r)
    {
        if (!valid(v) || v.revision != expected + 1 || !report_match(v, r))
            return {RoleCertificationStoreStatus::Invalid, 0, {}, "terminal checkpoint/report binding invalid"};
        std::lock_guard lock(mutex_);
        auto k = key(v.metadata.identity.tenant_id, v.workflow_id);
        auto it = checkpoints_.find(k);
        if (it == checkpoints_.end())
            return {RoleCertificationStoreStatus::NotFound, 0, {}, {}};
        if (it->second.revision != expected)
            return {RoleCertificationStoreStatus::RevisionConflict, it->second.revision, {}, {}};
        if (!immutable(it->second.checkpoint, v))
            return {RoleCertificationStoreStatus::Invalid, expected, {}, "immutable bindings changed"};
        if (reports_.count(k))
            return {RoleCertificationStoreStatus::AlreadyExists, expected, {}, {}};
        it->second = {v, v.revision};
        reports_[k] = {r, 1};
        return {RoleCertificationStoreStatus::Committed, v.revision, encode(r).at("canonical_digest"), {}};
    }
    std::optional<StoredRoleCertificationReport> InMemoryRoleCertificationStore::load_report(std::string_view t, std::string_view w)
    {
        std::lock_guard lock(mutex_);
        auto it = reports_.find(key(t, w));
        return it == reports_.end() ? std::nullopt : std::optional(it->second);
    }

    SQLiteRoleCertificationStore::SQLiteRoleCertificationStore(std::string path, SQLiteRoleCertificationStoreOptions options) : path_(std::move(path)), options_(options)
    {
        if (path_.empty())
            throw std::invalid_argument("live certification store path must not be empty");
        std::filesystem::path file(path_);
        std::error_code ec;
        if (file.has_parent_path())
            std::filesystem::create_directories(file.parent_path(), ec);
        if (ec)
            throw std::runtime_error(ec.message());
        sqlite3 *db = nullptr;
        if (sqlite3_open_v2(path_.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
        {
            std::string m = db ? sqlite3_errmsg(db) : "sqlite open failed";
            if (db)
                sqlite3_close(db);
            throw std::runtime_error(m);
        }
        db_ = db;
        sqlite3_busy_timeout(db, options.busy_timeout_ms);
        sqlite::exec(db, "PRAGMA journal_mode=WAL");
        sqlite::exec(db, "PRAGMA synchronous=FULL");
        migrate();
#if !defined(_WIN32)
        if (options.require_private_permissions)
        {
            std::filesystem::permissions(file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, ec);
            if (ec)
                throw std::runtime_error(ec.message());
        }
#endif
    }
    SQLiteRoleCertificationStore::~SQLiteRoleCertificationStore()
    {
        if (db_)
            sqlite3_close(sqlite::database(db_));
    }
    void SQLiteRoleCertificationStore::migrate()
    {
        auto *db = sqlite::database(db_);
        sqlite::exec(db, "CREATE TABLE IF NOT EXISTS live_cert_schema_version(version INTEGER PRIMARY KEY,applied_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)");
        sqlite::exec(db, "INSERT OR IGNORE INTO live_cert_schema_version(version) VALUES(1)");
        sqlite::exec(db, "CREATE TABLE IF NOT EXISTS live_cert_checkpoints(tenant_id TEXT NOT NULL,workflow_id TEXT NOT NULL,task_id TEXT NOT NULL,revision INTEGER NOT NULL,state TEXT NOT NULL,checkpoint_json TEXT NOT NULL,checkpoint_digest TEXT NOT NULL,updated_at TEXT NOT NULL,PRIMARY KEY(tenant_id,workflow_id))");
        sqlite::exec(db, "CREATE TABLE IF NOT EXISTS live_cert_reports(tenant_id TEXT NOT NULL,workflow_id TEXT NOT NULL,revision INTEGER NOT NULL,report_json TEXT NOT NULL,report_digest TEXT NOT NULL,created_at TEXT NOT NULL,PRIMARY KEY(tenant_id,workflow_id))");
    }
    RoleCertificationStoreCommit SQLiteRoleCertificationStore::create(const RoleCertificationCheckpoint &v)
    {
        if (!valid(v) || v.revision != 1)
            return {RoleCertificationStoreStatus::Invalid, 0, {}, "invalid initial checkpoint"};
        auto doc = encode(v);
        auto dg = doc.at("canonical_digest").get<std::string>();
        std::lock_guard lock(mutex_);
        auto *db = sqlite::database(db_);
        sqlite::Statement s(db, "INSERT INTO live_cert_checkpoints(tenant_id,workflow_id,task_id,revision,state,checkpoint_json,checkpoint_digest,updated_at) VALUES(?,?,?,?,?,?,?,?)");
        sqlite::bind_text(s.get(), 1, v.metadata.identity.tenant_id);
        sqlite::bind_text(s.get(), 2, v.workflow_id);
        sqlite::bind_text(s.get(), 3, v.metadata.identity.task_id);
        sqlite3_bind_int64(s.get(), 4, v.revision);
        sqlite::bind_text(s.get(), 5, role_certification_state_name(v.state));
        sqlite::bind_text(s.get(), 6, doc.dump());
        sqlite::bind_text(s.get(), 7, dg);
        sqlite::bind_text(s.get(), 8, v.updated_at);
        int rc = sqlite3_step(s.get());
        if (rc == SQLITE_CONSTRAINT)
            return {RoleCertificationStoreStatus::AlreadyExists, 0, {}, {}};
        if (rc != SQLITE_DONE)
            return fail(db, rc);
        return {RoleCertificationStoreStatus::Committed, v.revision, dg, {}};
    }
    std::optional<StoredRoleCertificationCheckpoint> SQLiteRoleCertificationStore::load(std::string_view t, std::string_view w)
    {
        std::lock_guard lock(mutex_);
        auto *db = sqlite::database(db_);
        sqlite::Statement s(db, "SELECT revision,checkpoint_json,checkpoint_digest FROM live_cert_checkpoints WHERE tenant_id=? AND workflow_id=?");
        sqlite::bind_text(s.get(), 1, t);
        sqlite::bind_text(s.get(), 2, w);
        int rc = sqlite3_step(s.get());
        if (rc == SQLITE_DONE)
            return std::nullopt;
        if (rc != SQLITE_ROW)
            throw std::runtime_error(sqlite3_errmsg(db));
        auto rev = static_cast<std::uint64_t>(sqlite3_column_int64(s.get(), 0));
        auto text = sqlite::column_text(s.get(), 1);
        auto dg = sqlite::column_text(s.get(), 2);
        auto value = decode_role_certification_checkpoint(nlohmann::json::parse(text));
        if (!value || value->revision != rev || encode(*value).at("canonical_digest") != dg)
            throw std::runtime_error("stored live checkpoint is corrupt");
        return StoredRoleCertificationCheckpoint{*value, rev};
    }
    RoleCertificationStoreCommit SQLiteRoleCertificationStore::compare_exchange(const RoleCertificationCheckpoint &v, std::uint64_t expected)
    {
        if (!valid(v) || v.revision != expected + 1)
            return {RoleCertificationStoreStatus::Invalid, 0, {}, "revision must advance once"};
        auto existing = load(v.metadata.identity.tenant_id, v.workflow_id);
        if (!existing)
            return {RoleCertificationStoreStatus::NotFound, 0, {}, {}};
        if (existing->revision != expected)
            return {RoleCertificationStoreStatus::RevisionConflict, existing->revision, {}, {}};
        if (!immutable(existing->checkpoint, v))
            return {RoleCertificationStoreStatus::Invalid, expected, {}, "immutable bindings changed"};
        auto doc = encode(v);
        auto dg = doc.at("canonical_digest").get<std::string>();
        std::lock_guard lock(mutex_);
        auto *db = sqlite::database(db_);
        sqlite::Statement s(db, "UPDATE live_cert_checkpoints SET revision=?,state=?,checkpoint_json=?,checkpoint_digest=?,updated_at=? WHERE tenant_id=? AND workflow_id=? AND revision=?");
        sqlite3_bind_int64(s.get(), 1, v.revision);
        sqlite::bind_text(s.get(), 2, role_certification_state_name(v.state));
        sqlite::bind_text(s.get(), 3, doc.dump());
        sqlite::bind_text(s.get(), 4, dg);
        sqlite::bind_text(s.get(), 5, v.updated_at);
        sqlite::bind_text(s.get(), 6, v.metadata.identity.tenant_id);
        sqlite::bind_text(s.get(), 7, v.workflow_id);
        sqlite3_bind_int64(s.get(), 8, expected);
        int rc = sqlite3_step(s.get());
        if (rc != SQLITE_DONE)
            return fail(db, rc);
        if (sqlite3_changes(db) != 1)
            return {RoleCertificationStoreStatus::RevisionConflict, expected, {}, {}};
        return {RoleCertificationStoreStatus::Committed, v.revision, dg, {}};
    }
    RoleCertificationStoreCommit SQLiteRoleCertificationStore::commit_report(const RoleCertificationCheckpoint &v, std::uint64_t expected, const RoleCertificationReport &r)
    {
        if (!valid(v) || v.revision != expected + 1 || !report_match(v, r))
            return {RoleCertificationStoreStatus::Invalid, 0, {}, "terminal checkpoint/report binding invalid"};
        auto existing = load(v.metadata.identity.tenant_id, v.workflow_id);
        if (!existing)
            return {RoleCertificationStoreStatus::NotFound, 0, {}, {}};
        if (existing->revision != expected)
            return {RoleCertificationStoreStatus::RevisionConflict, existing->revision, {}, {}};
        if (!immutable(existing->checkpoint, v))
            return {RoleCertificationStoreStatus::Invalid, expected, {}, "immutable bindings changed"};
        auto cp = encode(v), rp = encode(r);
        std::lock_guard lock(mutex_);
        auto *db = sqlite::database(db_);
        try
        {
            sqlite::exec(db, "BEGIN IMMEDIATE");
            sqlite::Statement u(db, "UPDATE live_cert_checkpoints SET revision=?,state=?,checkpoint_json=?,checkpoint_digest=?,updated_at=? WHERE tenant_id=? AND workflow_id=? AND revision=?");
            sqlite3_bind_int64(u.get(), 1, v.revision);
            sqlite::bind_text(u.get(), 2, role_certification_state_name(v.state));
            sqlite::bind_text(u.get(), 3, cp.dump());
            sqlite::bind_text(u.get(), 4, cp.at("canonical_digest").get<std::string>());
            sqlite::bind_text(u.get(), 5, v.updated_at);
            sqlite::bind_text(u.get(), 6, v.metadata.identity.tenant_id);
            sqlite::bind_text(u.get(), 7, v.workflow_id);
            sqlite3_bind_int64(u.get(), 8, expected);
            int rc = sqlite3_step(u.get());
            if (rc != SQLITE_DONE || sqlite3_changes(db) != 1)
            {
                sqlite::exec(db, "ROLLBACK");
                return rc == SQLITE_DONE ? RoleCertificationStoreCommit{RoleCertificationStoreStatus::RevisionConflict, expected, {}, {}} : fail(db, rc);
            }
            sqlite::Statement i(db, "INSERT INTO live_cert_reports(tenant_id,workflow_id,revision,report_json,report_digest,created_at) VALUES(?,?,?,?,?,?)");
            sqlite::bind_text(i.get(), 1, v.metadata.identity.tenant_id);
            sqlite::bind_text(i.get(), 2, v.workflow_id);
            sqlite3_bind_int64(i.get(), 3, 1);
            sqlite::bind_text(i.get(), 4, rp.dump());
            sqlite::bind_text(i.get(), 5, rp.at("canonical_digest").get<std::string>());
            sqlite::bind_text(i.get(), 6, r.issued_at);
            rc = sqlite3_step(i.get());
            if (rc != SQLITE_DONE)
            {
                sqlite::exec(db, "ROLLBACK");
                return rc == SQLITE_CONSTRAINT ? RoleCertificationStoreCommit{RoleCertificationStoreStatus::AlreadyExists, expected, {}, {}} : fail(db, rc);
            }
            sqlite::exec(db, "COMMIT");
            return {RoleCertificationStoreStatus::Committed, v.revision, rp.at("canonical_digest"), {}};
        }
        catch (...)
        {
            try
            {
                sqlite::exec(db, "ROLLBACK");
            }
            catch (...)
            {
            }
            throw;
        }
    }
    std::optional<StoredRoleCertificationReport> SQLiteRoleCertificationStore::load_report(std::string_view t, std::string_view w)
    {
        std::lock_guard lock(mutex_);
        auto *db = sqlite::database(db_);
        sqlite::Statement s(db, "SELECT revision,report_json,report_digest FROM live_cert_reports WHERE tenant_id=? AND workflow_id=?");
        sqlite::bind_text(s.get(), 1, t);
        sqlite::bind_text(s.get(), 2, w);
        int rc = sqlite3_step(s.get());
        if (rc == SQLITE_DONE)
            return std::nullopt;
        if (rc != SQLITE_ROW)
            throw std::runtime_error(sqlite3_errmsg(db));
        auto rev = static_cast<std::uint64_t>(sqlite3_column_int64(s.get(), 0));
        auto text = sqlite::column_text(s.get(), 1);
        auto dg = sqlite::column_text(s.get(), 2);
        auto value = decode_role_certification_report(nlohmann::json::parse(text));
        if (!value || encode(*value).at("canonical_digest") != dg)
            throw std::runtime_error("stored live report is corrupt");
        return StoredRoleCertificationReport{*value, rev};
    }

} // namespace agent_framework::live
