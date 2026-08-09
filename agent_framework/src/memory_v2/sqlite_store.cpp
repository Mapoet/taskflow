#include "agent/memory_v2/store.hpp"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

#include <sqlite3.h>

#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::memory_v2
{
    namespace
    {
        using json = nlohmann::json;
        namespace sqlite = internal::sqlite;
        CommitResult failure(sqlite3 *d, int r) { return {r == SQLITE_BUSY || r == SQLITE_LOCKED ? CommitStatus::Busy : CommitStatus::Error, 0, sqlite3_errmsg(d)}; }
        std::optional<MemoryRecord> decode_record(std::string_view value)
        {
            try
            {
                return decode_memory_record(json::parse(value));
            }
            catch (...)
            {
                return std::nullopt;
            }
        }
        bool level_in(MemoryLevel level, const std::vector<MemoryLevel> &values) { return values.empty() || std::find(values.begin(), values.end(), level) != values.end(); }
        bool path_contains(std::string_view parent, std::string_view child)
        {
            if (parent.empty())
                return true;
            const auto base = std::filesystem::path(parent).lexically_normal();
            const auto target = std::filesystem::path(child).lexically_normal();
            auto b = base.begin();
            auto t = target.begin();
            for (; b != base.end(); ++b, ++t)
                if (t == target.end() || *b != *t)
                    return false;
            return true;
        }
        bool governed(const MemoryRecord &r) { return r.status == MemoryStatus::Authoritative || r.authority == Authority::Authoritative || r.scope.level == MemoryLevel::System || r.scope.level == MemoryLevel::Organization; }
        bool validate(const MemoryRecord &r, std::string_view approval, std::string *error)
        {
            if (r.record_id.empty() || r.scope.tenant_id.empty() || r.metadata.identity.tenant_id != r.scope.tenant_id)
            {
                if (error)
                    *error = "record, tenant, and matching metadata identity are required";
                return false;
            }
            if (r.revision == 0)
            {
                if (error)
                    *error = "revision must be positive";
                return false;
            }
            if (r.status == MemoryStatus::Authoritative && r.authority != Authority::Authoritative)
            {
                if (error)
                    *error = "authoritative status requires authoritative authority";
                return false;
            }
            if (governed(r) && approval.empty())
            {
                if (error)
                    *error = "governed memory write requires approval";
                return false;
            }
            return true;
        }
    }

    bool can_transition(MemoryStatus from, MemoryStatus to) noexcept
    {
        if (from == to)
            return false;
        switch (from)
        {
        case MemoryStatus::Raw:
            return to == MemoryStatus::Candidate || to == MemoryStatus::Rejected || to == MemoryStatus::Tombstoned;
        case MemoryStatus::Candidate:
            return to == MemoryStatus::Verified || to == MemoryStatus::Rejected || to == MemoryStatus::Tombstoned || to == MemoryStatus::Expired;
        case MemoryStatus::Verified:
            return to == MemoryStatus::Authoritative || to == MemoryStatus::Rejected || to == MemoryStatus::Superseded || to == MemoryStatus::Tombstoned || to == MemoryStatus::Expired;
        case MemoryStatus::Authoritative:
            return to == MemoryStatus::Superseded || to == MemoryStatus::Tombstoned || to == MemoryStatus::Expired;
        case MemoryStatus::Rejected:
        case MemoryStatus::Superseded:
        case MemoryStatus::Tombstoned:
        case MemoryStatus::Expired:
            return false;
        }
        return false;
    }
    bool memory_visible_to(const MemoryRecord &r, const MemoryQuery &q) noexcept
    {
        if (r.scope.tenant_id != q.subject.tenant_id || !level_in(r.scope.level, q.levels))
            return false;
        if (!r.scope.organization_id.empty() && r.scope.organization_id != q.subject.organization_id)
            return false;
        if (!r.scope.principal_id.empty() && r.scope.principal_id != q.subject.principal_id)
            return false;
        if (!r.scope.agent_id.empty() && r.scope.agent_id != q.subject.agent_id)
            return false;
        if (!r.scope.project_id.empty() && r.scope.project_id != q.subject.project_id)
            return false;
        if (!r.scope.workspace_id.empty() && r.scope.workspace_id != q.subject.workspace_id)
            return false;
        if (!r.scope.path_scope.empty() && !path_contains(r.scope.path_scope, q.subject.path_scope))
            return false;
        if (!r.scope.task_id.empty() && r.scope.task_id != q.subject.task_id)
            return false;
        if (!r.scope.run_id.empty() && r.scope.run_id != q.subject.run_id)
            return false;
        if (!r.scope.turn_id.empty() && r.scope.turn_id != q.subject.turn_id)
            return false;
        if (!r.acl_principals.empty() && std::find(r.acl_principals.begin(), r.acl_principals.end(), q.principal_id) == r.acl_principals.end())
            return false;
        return r.status != MemoryStatus::Rejected && r.status != MemoryStatus::Superseded && r.status != MemoryStatus::Tombstoned && r.status != MemoryStatus::Expired;
    }

    SQLiteMemoryStore::SQLiteMemoryStore(std::string path, SQLiteMemoryStoreOptions options) : path_(std::move(path)), options_(options)
    {
        if (path_.empty())
            throw std::invalid_argument("memory store path must not be empty");
        std::filesystem::path p(path_);
        std::error_code e;
        if (p.has_parent_path())
            std::filesystem::create_directories(p.parent_path(), e);
        if (e)
            throw std::runtime_error(e.message());
        sqlite3 *d = nullptr;
        int r = sqlite3_open_v2(path_.c_str(), &d, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
        if (r != SQLITE_OK)
        {
            std::string m = d ? sqlite3_errmsg(d) : "sqlite open failed";
            if (d)
                sqlite3_close(d);
            throw std::runtime_error(m);
        }
        db_ = d;
        sqlite3_busy_timeout(d, options_.busy_timeout_ms);
        sqlite::exec(d, "PRAGMA journal_mode=WAL");
        sqlite::exec(d, "PRAGMA synchronous=FULL");
        migrate();
#if !defined(_WIN32)
        if (options_.require_private_permissions)
        {
            std::filesystem::permissions(p, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, e);
            if (e)
                throw std::runtime_error(e.message());
        }
#endif
    }
    SQLiteMemoryStore::~SQLiteMemoryStore()
    {
        if (db_)
            sqlite3_close(sqlite::database(db_));
    }
    void SQLiteMemoryStore::migrate()
    {
        auto *d = sqlite::database(db_);
        sqlite::Transaction t(d);
        sqlite::exec(d, "CREATE TABLE IF NOT EXISTS memory_schema_version(version INTEGER PRIMARY KEY,applied_at TEXT NOT NULL)");
        int v = 0;
        {
            sqlite::Statement s(d, "SELECT COALESCE(MAX(version),0) FROM memory_schema_version");
            if (sqlite3_step(s.get()) == SQLITE_ROW)
                v = sqlite3_column_int(s.get(), 0);
        }
        if (v > 1)
            throw std::runtime_error("memory schema newer than binary");
        if (v == 0)
        {
            sqlite::exec(d, "CREATE TABLE memory_records(record_id TEXT PRIMARY KEY,tenant_id TEXT NOT NULL,revision INTEGER NOT NULL,level INTEGER NOT NULL,status INTEGER NOT NULL,document_json TEXT NOT NULL,updated_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%fZ','now')))");
            sqlite::exec(d, "CREATE TABLE memory_history(record_id TEXT NOT NULL,revision INTEGER NOT NULL,document_json TEXT NOT NULL,approval_id TEXT NOT NULL,created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%fZ','now')),PRIMARY KEY(record_id,revision))");
            sqlite::exec(d, "CREATE INDEX memory_scope_idx ON memory_records(tenant_id,level,status)");
            sqlite::exec(d, "CREATE TABLE memory_generation(singleton INTEGER PRIMARY KEY CHECK(singleton=1),generation INTEGER NOT NULL)");
            sqlite::exec(d, "INSERT INTO memory_generation VALUES(1,0)");
            sqlite::exec(d, "INSERT INTO memory_schema_version VALUES(1,strftime('%Y-%m-%dT%H:%M:%fZ','now'))");
        }
        t.commit();
    }
    CommitResult SQLiteMemoryStore::append(const MemoryRecord &r, std::string_view approval)
    {
        std::lock_guard l(mutex_);
        std::string e;
        if (!validate(r, approval, &e) || r.revision != 1)
            return {CommitStatus::Invalid, 0, e.empty() ? "initial revision must be 1" : e};
        auto *d = sqlite::database(db_);
        try
        {
            sqlite::Transaction t(d);
            auto doc = encode(r).dump();
            sqlite::Statement h(d, "INSERT INTO memory_history(record_id,revision,document_json,approval_id)VALUES(?,?,?,?)");
            sqlite::bind_text(h.get(), 1, r.record_id);
            sqlite3_bind_int64(h.get(), 2, 1);
            sqlite::bind_text(h.get(), 3, doc);
            sqlite::bind_text(h.get(), 4, approval);
            int rc = sqlite3_step(h.get());
            if (rc == SQLITE_CONSTRAINT || rc == SQLITE_CONSTRAINT_PRIMARYKEY ||
                rc == SQLITE_CONSTRAINT_UNIQUE)
            {
                const int extended = sqlite3_extended_errcode(d);
                if (extended == SQLITE_CONSTRAINT_PRIMARYKEY || extended == SQLITE_CONSTRAINT_UNIQUE)
                    return {CommitStatus::AlreadyExists, 0, "record exists"};
                return failure(d, rc);
            }
            if (rc != SQLITE_DONE)
                return failure(d, rc);
            sqlite::Statement c(d, "INSERT INTO memory_records(record_id,tenant_id,revision,level,status,document_json)VALUES(?,?,?,?,?,?)");
            sqlite::bind_text(c.get(), 1, r.record_id);
            sqlite::bind_text(c.get(), 2, r.scope.tenant_id);
            sqlite3_bind_int64(c.get(), 3, 1);
            sqlite3_bind_int(c.get(), 4, static_cast<int>(r.scope.level));
            sqlite3_bind_int(c.get(), 5, static_cast<int>(r.status));
            sqlite::bind_text(c.get(), 6, doc);
            rc = sqlite3_step(c.get());
            if (rc != SQLITE_DONE)
                return failure(d, rc);
            sqlite::exec(d, "UPDATE memory_generation SET generation=generation+1 WHERE singleton=1");
            t.commit();
            return {CommitStatus::Committed, 1, {}};
        }
        catch (const std::exception &x)
        {
            return {CommitStatus::Error, 0, x.what()};
        }
    }
    CommitResult SQLiteMemoryStore::revise(const MemoryRecord &r, std::uint64_t expected, std::string_view approval)
    {
        std::lock_guard l(mutex_);
        std::string e;
        if (!validate(r, approval, &e) || r.revision != expected + 1)
            return {CommitStatus::Invalid, 0, e.empty() ? "revision must be expected+1" : e};
        auto *d = sqlite::database(db_);
        try
        {
            sqlite::Transaction t(d);
            MemoryStatus old;
            std::uint64_t actual;
            {
                sqlite::Statement q(d, "SELECT revision,document_json FROM memory_records WHERE record_id=?");
                sqlite::bind_text(q.get(), 1, r.record_id);
                if (sqlite3_step(q.get()) != SQLITE_ROW)
                    return {CommitStatus::NotFound, 0, "record not found"};
                actual = sqlite3_column_int64(q.get(), 0);
                auto prior = decode_record(sqlite::column_text(q.get(), 1));
                if (!prior)
                    return {CommitStatus::Error, actual, "corrupt memory record"};
                old = prior->status;
            }
            if (actual != expected)
                return {CommitStatus::RevisionConflict, actual, "revision conflict"};
            if (!can_transition(old, r.status))
                return {CommitStatus::Invalid, actual, "illegal memory lifecycle transition"};
            auto doc = encode(r).dump();
            sqlite::Statement h(d, "INSERT INTO memory_history(record_id,revision,document_json,approval_id)VALUES(?,?,?,?)");
            sqlite::bind_text(h.get(), 1, r.record_id);
            sqlite3_bind_int64(h.get(), 2, r.revision);
            sqlite::bind_text(h.get(), 3, doc);
            sqlite::bind_text(h.get(), 4, approval);
            int rc = sqlite3_step(h.get());
            if (rc != SQLITE_DONE)
                return failure(d, rc);
            sqlite::Statement u(d, "UPDATE memory_records SET revision=?,level=?,status=?,document_json=?,updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE record_id=? AND revision=?");
            sqlite3_bind_int64(u.get(), 1, r.revision);
            sqlite3_bind_int(u.get(), 2, static_cast<int>(r.scope.level));
            sqlite3_bind_int(u.get(), 3, static_cast<int>(r.status));
            sqlite::bind_text(u.get(), 4, doc);
            sqlite::bind_text(u.get(), 5, r.record_id);
            sqlite3_bind_int64(u.get(), 6, expected);
            rc = sqlite3_step(u.get());
            if (rc != SQLITE_DONE)
                return failure(d, rc);
            if (sqlite3_changes(d) != 1)
                return {CommitStatus::RevisionConflict, actual, "concurrent revision"};
            sqlite::exec(d, "UPDATE memory_generation SET generation=generation+1 WHERE singleton=1");
            t.commit();
            return {CommitStatus::Committed, r.revision, {}};
        }
        catch (const std::exception &x)
        {
            return {CommitStatus::Error, 0, x.what()};
        }
    }
    std::optional<MemoryRecord> SQLiteMemoryStore::current(std::string_view id)
    {
        std::lock_guard l(mutex_);
        sqlite::Statement s(sqlite::database(db_), "SELECT document_json FROM memory_records WHERE record_id=?");
        sqlite::bind_text(s.get(), 1, id);
        if (sqlite3_step(s.get()) != SQLITE_ROW)
            return std::nullopt;
        auto r = decode_record(sqlite::column_text(s.get(), 0));
        if (!r)
            throw std::runtime_error("corrupt memory record");
        return r;
    }
    std::vector<MemoryRecord> SQLiteMemoryStore::history(std::string_view id)
    {
        std::lock_guard l(mutex_);
        std::vector<MemoryRecord> o;
        sqlite::Statement s(sqlite::database(db_), "SELECT document_json FROM memory_history WHERE record_id=? ORDER BY revision");
        sqlite::bind_text(s.get(), 1, id);
        while (sqlite3_step(s.get()) == SQLITE_ROW)
        {
            auto r = decode_record(sqlite::column_text(s.get(), 0));
            if (!r)
                throw std::runtime_error("corrupt memory history");
            o.push_back(std::move(*r));
        }
        return o;
    }
    std::vector<MemoryRecord> SQLiteMemoryStore::query(const MemoryQuery &q)
    {
        std::lock_guard l(mutex_);
        std::vector<MemoryRecord> o;
        sqlite::Statement s(sqlite::database(db_), "SELECT document_json FROM memory_records WHERE tenant_id=? ORDER BY level,record_id");
        sqlite::bind_text(s.get(), 1, q.subject.tenant_id);
        while (sqlite3_step(s.get()) == SQLITE_ROW)
        {
            auto r = decode_record(sqlite::column_text(s.get(), 0));
            if (!r)
                throw std::runtime_error("corrupt memory record");
            if (memory_visible_to(*r, q))
            {
                o.push_back(std::move(*r));
                if (q.limit != 0 && o.size() >= q.limit)
                    break;
            }
        }
        return o;
    }
    std::uint64_t SQLiteMemoryStore::generation()
    {
        std::lock_guard l(mutex_);
        sqlite::Statement s(sqlite::database(db_), "SELECT generation FROM memory_generation WHERE singleton=1");
        return sqlite3_step(s.get()) == SQLITE_ROW ? static_cast<std::uint64_t>(sqlite3_column_int64(s.get(), 0)) : 0;
    }
} // namespace agent_framework::memory_v2
