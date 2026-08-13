#include "agent/agent_template/registry.hpp"

#include <filesystem>
#include <stdexcept>

#include <sqlite3.h>

#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::agent_template
{
    namespace
    {
        namespace sqlite = internal::sqlite;
        using json = nlohmann::json;
        RegistryResult failure(sqlite3 *db, int code)
        {
            return {code == SQLITE_BUSY || code == SQLITE_LOCKED ? RegistryStatus::Busy
                                                                 : RegistryStatus::Error,
                    0,
                    {},
                    sqlite3_errmsg(db)};
        }
        bool valid_template(const AgentTemplate &v)
        {
            return !v.metadata.identity.tenant_id.empty() && !v.template_id.empty() && v.revision > 0;
        }
        bool valid_invocation(const AgentTemplateInvocation &v)
        {
            return !v.metadata.identity.tenant_id.empty() && !v.invocation_id.empty() &&
                   v.revision > 0 && !v.template_ref.template_id.empty() &&
                   v.template_ref.revision > 0 && !v.template_ref.digest.empty() &&
                   !v.plan_digest.empty() && !v.skill_session_digest.empty() &&
                   !v.capability_snapshot_digest.empty() && !v.deployment_generation.empty();
        }
        std::optional<AgentTemplate> decode_template_row(sqlite3_stmt *statement, int column)
        {
            std::vector<contracts::ContractIssue> issues;
            auto value = decode_agent_template(json::parse(sqlite::column_text(statement, column)), {}, &issues);
            if (!value)
            {
                std::string message = "stored agent template is corrupt";
                if (!issues.empty())
                    message += ": " + issues.front().code + " " + issues.front().message;
                throw std::runtime_error(message);
            }
            return value;
        }
    }

    SQLiteAgentTemplateRegistry::SQLiteAgentTemplateRegistry(
        std::string path, SQLiteRegistryOptions options)
        : path_(std::move(path)), options_(options)
    {
        if (path_.empty())
            throw std::invalid_argument("agent template registry path is empty");
        const std::filesystem::path file(path_);
        std::error_code error;
        if (file.has_parent_path())
            std::filesystem::create_directories(file.parent_path(), error);
        if (error)
            throw std::runtime_error("unable to create registry directory: " + error.message());
        sqlite3 *db = nullptr;
        const int code = sqlite3_open_v2(path_.c_str(), &db,
                                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
        if (code != SQLITE_OK)
        {
            const std::string message = db ? sqlite3_errmsg(db) : "sqlite open failed";
            if (db)
                sqlite3_close(db);
            throw std::runtime_error(message);
        }
        db_ = db;
        sqlite3_busy_timeout(db, options_.busy_timeout_ms);
        sqlite::exec(db, "PRAGMA journal_mode=WAL");
        sqlite::exec(db, "PRAGMA synchronous=FULL");
        migrate();
#if !defined(_WIN32)
        if (options_.require_private_permissions)
        {
            std::filesystem::permissions(file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, error);
            if (error)
                throw std::runtime_error("unable to set registry permissions: " + error.message());
        }
#endif
    }
    SQLiteAgentTemplateRegistry::~SQLiteAgentTemplateRegistry()
    {
        if (db_)
            sqlite3_close(sqlite::database(db_));
    }
    void SQLiteAgentTemplateRegistry::migrate()
    {
        auto *db = sqlite::database(db_);
        sqlite::Transaction tx(db);
        sqlite::exec(db, "CREATE TABLE IF NOT EXISTS agent_template_schema(version INTEGER PRIMARY KEY,applied_at TEXT NOT NULL)");
        int version = 0;
        {
            sqlite::Statement q(db, "SELECT COALESCE(MAX(version),0) FROM agent_template_schema");
            if (sqlite::step(q.get()) == SQLITE_ROW)
                version = sqlite::column_int(q.get(), 0);
        }
        if (version > 1)
            throw std::runtime_error("agent template registry schema is newer than binary");
        if (version == 0)
        {
            sqlite::exec(db, "CREATE TABLE agent_templates(tenant_id TEXT NOT NULL,template_id TEXT NOT NULL,revision INTEGER NOT NULL,digest TEXT NOT NULL,document_json TEXT NOT NULL,created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%fZ','now')),PRIMARY KEY(tenant_id,template_id,revision))");
            sqlite::exec(db, "CREATE TABLE agent_template_invocations(tenant_id TEXT NOT NULL,invocation_id TEXT NOT NULL,store_revision INTEGER NOT NULL,digest TEXT NOT NULL,document_json TEXT NOT NULL,updated_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%fZ','now')),PRIMARY KEY(tenant_id,invocation_id))");
            sqlite::exec(db, "INSERT INTO agent_template_schema VALUES(1,strftime('%Y-%m-%dT%H:%M:%fZ','now'))");
        }
        tx.commit();
    }

    RegistryResult SQLiteAgentTemplateRegistry::publish(const AgentTemplate &v)
    {
        std::lock_guard lock(mutex_);
        if (!valid_template(v))
            return {RegistryStatus::Invalid, 0, {}, "invalid template"};
        auto *db = sqlite::database(db_);
        const auto doc = encode(v);
        const auto digest = doc.at("canonical_digest").get<std::string>();
        sqlite::Statement s(db, "INSERT INTO agent_templates(tenant_id,template_id,revision,digest,document_json) VALUES(?,?,?,?,?)");
        sqlite::bind_text(s.get(), 1, v.metadata.identity.tenant_id);
        sqlite::bind_text(s.get(), 2, v.template_id);
        sqlite::bind_uint64(s.get(), 3, v.revision);
        sqlite::bind_text(s.get(), 4, digest);
        sqlite::bind_text(s.get(), 5, contracts::canonical_json(doc));
        const int code = sqlite::step(s.get());
        if (code == SQLITE_DONE)
            return {RegistryStatus::Committed, v.revision, digest, {}};
        if (code != SQLITE_CONSTRAINT)
            return failure(db, code);
        sqlite::Statement q(db, "SELECT digest FROM agent_templates WHERE tenant_id=? AND template_id=? AND revision=?");
        sqlite::bind_text(q.get(), 1, v.metadata.identity.tenant_id);
        sqlite::bind_text(q.get(), 2, v.template_id);
        sqlite::bind_uint64(q.get(), 3, v.revision);
        if (sqlite::step(q.get()) != SQLITE_ROW)
            return {RegistryStatus::Error, 0, digest, "constraint without row"};
        const bool same = sqlite::column_text(q.get(), 0) == digest;
        return {same ? RegistryStatus::AlreadyExists : RegistryStatus::RevisionConflict, v.revision, digest, same ? "" : "immutable template revision mismatch"};
    }
    std::optional<AgentTemplate> SQLiteAgentTemplateRegistry::load(std::string_view tenant, std::string_view id, std::uint64_t revision)
    {
        std::lock_guard lock(mutex_);
        auto *db = sqlite::database(db_);
        sqlite::Statement q(db, "SELECT document_json FROM agent_templates WHERE tenant_id=? AND template_id=? AND revision=?");
        sqlite::bind_text(q.get(), 1, tenant);
        sqlite::bind_text(q.get(), 2, id);
        sqlite::bind_uint64(q.get(), 3, revision);
        if (sqlite::step(q.get()) != SQLITE_ROW)
            return std::nullopt;
        return decode_template_row(q.get(), 0);
    }
    std::optional<AgentTemplate> SQLiteAgentTemplateRegistry::latest(std::string_view tenant, std::string_view id)
    {
        std::lock_guard lock(mutex_);
        auto *db = sqlite::database(db_);
        sqlite::Statement q(db, "SELECT document_json FROM agent_templates WHERE tenant_id=? AND template_id=? ORDER BY revision DESC LIMIT 1");
        sqlite::bind_text(q.get(), 1, tenant);
        sqlite::bind_text(q.get(), 2, id);
        if (sqlite::step(q.get()) != SQLITE_ROW)
            return std::nullopt;
        return decode_template_row(q.get(), 0);
    }
    RegistryResult SQLiteAgentTemplateRegistry::create_invocation(const AgentTemplateInvocation &v)
    {
        std::lock_guard lock(mutex_);
        if (!valid_invocation(v))
            return {RegistryStatus::Invalid, 0, {}, "invocation pins incomplete"};
        auto *db = sqlite::database(db_);
        const auto doc = encode(v);
        const auto digest = doc.at("canonical_digest").get<std::string>();
        sqlite::Statement s(db, "INSERT INTO agent_template_invocations(tenant_id,invocation_id,store_revision,digest,document_json) VALUES(?,?,?,?,?)");
        sqlite::bind_text(s.get(), 1, v.metadata.identity.tenant_id);
        sqlite::bind_text(s.get(), 2, v.invocation_id);
        sqlite::bind_uint64(s.get(), 3, 1);
        sqlite::bind_text(s.get(), 4, digest);
        sqlite::bind_text(s.get(), 5, contracts::canonical_json(doc));
        const int code = sqlite::step(s.get());
        if (code == SQLITE_CONSTRAINT)
            return {RegistryStatus::AlreadyExists, 0, digest, "invocation exists"};
        if (code != SQLITE_DONE)
            return failure(db, code);
        return {RegistryStatus::Committed, 1, digest, {}};
    }
    RegistryResult SQLiteAgentTemplateRegistry::update_invocation(const AgentTemplateInvocation &v, std::uint64_t expected)
    {
        std::lock_guard lock(mutex_);
        if (!valid_invocation(v))
            return {RegistryStatus::Invalid, 0, {}, "invocation pins incomplete"};
        auto *db = sqlite::database(db_);
        const auto doc = encode(v);
        const auto digest = doc.at("canonical_digest").get<std::string>();
        sqlite::Statement s(db, "UPDATE agent_template_invocations SET store_revision=store_revision+1,digest=?,document_json=?,updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE tenant_id=? AND invocation_id=? AND store_revision=?");
        sqlite::bind_text(s.get(), 1, digest);
        sqlite::bind_text(s.get(), 2, contracts::canonical_json(doc));
        sqlite::bind_text(s.get(), 3, v.metadata.identity.tenant_id);
        sqlite::bind_text(s.get(), 4, v.invocation_id);
        sqlite::bind_uint64(s.get(), 5, expected);
        const int code = sqlite::step(s.get());
        if (code != SQLITE_DONE)
            return failure(db, code);
        if (sqlite::changes(db) != 1)
            return {RegistryStatus::RevisionConflict, expected, {}, "invocation CAS conflict"};
        return {RegistryStatus::Committed, expected + 1, digest, {}};
    }
    std::optional<StoredInvocation> SQLiteAgentTemplateRegistry::load_invocation(std::string_view tenant, std::string_view id)
    {
        std::lock_guard lock(mutex_);
        auto *db = sqlite::database(db_);
        sqlite::Statement q(db, "SELECT store_revision,document_json,updated_at FROM agent_template_invocations WHERE tenant_id=? AND invocation_id=?");
        sqlite::bind_text(q.get(), 1, tenant);
        sqlite::bind_text(q.get(), 2, id);
        if (sqlite::step(q.get()) != SQLITE_ROW)
            return std::nullopt;
        auto v = decode_template_invocation(json::parse(sqlite::column_text(q.get(), 1)));
        if (!v)
            throw std::runtime_error("stored invocation is corrupt");
        return StoredInvocation{std::move(*v), sqlite::column_uint64(q.get(), 0), sqlite::column_text(q.get(), 2)};
    }
} // namespace agent_framework::agent_template
