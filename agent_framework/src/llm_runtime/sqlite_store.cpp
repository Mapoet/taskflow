#include "agent/llm_runtime/store.hpp"

#include <filesystem>
#include <stdexcept>

#include <sqlite3.h>

namespace agent_framework::llm_runtime {
namespace {

using json = nlohmann::json;

class Statement {
public:
    Statement(sqlite3* db, const char* sql) {
        if(sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() { if(statement_) sqlite3_finalize(statement_); }
    sqlite3_stmt* get() const noexcept { return statement_; }
private:
    sqlite3_stmt* statement_{nullptr};
};

class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db) {
        char* error = nullptr;
        if(sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error ? error : sqlite3_errmsg(db_);
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }
    ~Transaction() { if(!done_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() {
        char* error = nullptr;
        if(sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error ? error : sqlite3_errmsg(db_);
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
        done_ = true;
    }
private:
    sqlite3* db_;
    bool done_{false};
};

sqlite3* database(void* value) { return static_cast<sqlite3*>(value); }

void exec(sqlite3* db, const char* sql) {
    char* error = nullptr;
    const int status = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if(status != SQLITE_OK) {
        const std::string message = error ? error : sqlite3_errmsg(db);
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

void bind_text(sqlite3_stmt* statement, int index, std::string_view value) {
    if(sqlite3_bind_text(statement, index, value.data(), static_cast<int>(value.size()),
                         SQLITE_TRANSIENT) != SQLITE_OK)
        throw std::runtime_error("sqlite text bind failed");
}

std::string column_text(sqlite3_stmt* statement, int index) {
    const auto* value = sqlite3_column_text(statement, index);
    return value ? reinterpret_cast<const char*>(value) : std::string();
}

RuntimeStoreResult sqlite_failure(sqlite3* db, int status) {
    return {status == SQLITE_BUSY || status == SQLITE_LOCKED ? RuntimeStoreStatus::Busy
                                                              : RuntimeStoreStatus::Error,
            0, {}, sqlite3_errmsg(db)};
}

bool terminal(InvocationState state) {
    return state == InvocationState::Succeeded || state == InvocationState::Failed ||
           state == InvocationState::Cancelled || state == InvocationState::ManualReview;
}

bool valid_transition(InvocationState from, InvocationState to) {
    if(from == to) return !terminal(from);
    if(from == InvocationState::Pending)
        return to == InvocationState::Running || to == InvocationState::Failed ||
               to == InvocationState::Cancelled;
    if(from == InvocationState::Running)
        return to == InvocationState::Succeeded || to == InvocationState::Failed ||
               to == InvocationState::Cancelled || to == InvocationState::ManualReview;
    return false;
}

template <typename T, typename Encoder>
RuntimeStoreResult publish_document(sqlite3* db, const char* table,
                                    std::string_view tenant, std::string_view id,
                                    std::string_view revision, const T& value,
                                    Encoder encoder) {
    const auto document = encoder(value);
    const auto digest = document.at("canonical_digest").template get<std::string>();
    const std::string insert_sql = std::string("INSERT INTO ") + table +
        "(tenant_id,document_id,revision,digest,document_json) VALUES(?,?,?,?,?)";
    Statement insert(db, insert_sql.c_str());
    bind_text(insert.get(), 1, tenant); bind_text(insert.get(), 2, id);
    bind_text(insert.get(), 3, revision); bind_text(insert.get(), 4, digest);
    bind_text(insert.get(), 5, contracts::canonical_json(document));
    const int status = sqlite3_step(insert.get());
    if(status == SQLITE_DONE) return {RuntimeStoreStatus::Committed, 1, digest, {}};
    if(status != SQLITE_CONSTRAINT) return sqlite_failure(db, status);
    const std::string query_sql = std::string("SELECT digest FROM ") + table +
        " WHERE tenant_id=? AND document_id=? AND revision=?";
    Statement query(db, query_sql.c_str());
    bind_text(query.get(), 1, tenant); bind_text(query.get(), 2, id); bind_text(query.get(), 3, revision);
    if(sqlite3_step(query.get()) != SQLITE_ROW)
        return {RuntimeStoreStatus::Error, 0, digest, "constraint without existing document"};
    const bool same = column_text(query.get(), 0) == digest;
    return {same ? RuntimeStoreStatus::AlreadyExists : RuntimeStoreStatus::RevisionConflict,
            0, digest, same ? std::string() : "immutable revision digest mismatch"};
}

template <typename T, typename Decoder>
std::optional<T> load_document(sqlite3* db, const char* table,
                               std::string_view tenant, std::string_view id,
                               std::string_view revision, Decoder decoder) {
    const std::string sql = std::string("SELECT document_json FROM ") + table +
        " WHERE tenant_id=? AND document_id=? AND revision=?";
    Statement query(db, sql.c_str());
    bind_text(query.get(), 1, tenant); bind_text(query.get(), 2, id); bind_text(query.get(), 3, revision);
    if(sqlite3_step(query.get()) != SQLITE_ROW) return std::nullopt;
    auto decoded = decoder(json::parse(column_text(query.get(), 0)));
    if(!decoded) throw std::runtime_error(std::string("corrupt ") + table + " document");
    return decoded;
}

}  // namespace

SQLiteLLMRuntimeStore::SQLiteLLMRuntimeStore(
    std::string path, SQLiteLLMRuntimeStoreOptions options)
    : path_(std::move(path)), options_(options) {
    if(path_.empty()) throw std::invalid_argument("LLM runtime store path must not be empty");
    const std::filesystem::path file(path_);
    std::error_code error;
    if(file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), error);
    if(error) throw std::runtime_error("unable to create LLM runtime store directory: " + error.message());
    sqlite3* db = nullptr;
    const int status = sqlite3_open_v2(path_.c_str(), &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if(status != SQLITE_OK) {
        const std::string message = db ? sqlite3_errmsg(db) : "sqlite open failed";
        if(db) sqlite3_close(db);
        throw std::runtime_error(message);
    }
    db_ = db;
    sqlite3_busy_timeout(db, options_.busy_timeout_ms);
    exec(db, "PRAGMA journal_mode=WAL");
    exec(db, "PRAGMA synchronous=FULL");
    migrate();
#if !defined(_WIN32)
    if(options_.require_private_permissions) {
        std::filesystem::permissions(file,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace, error);
        if(error) throw std::runtime_error("unable to set private LLM runtime store permissions: " + error.message());
    }
#endif
}

SQLiteLLMRuntimeStore::~SQLiteLLMRuntimeStore() {
    if(db_) sqlite3_close(database(db_));
}

void SQLiteLLMRuntimeStore::migrate() {
    auto* db = database(db_);
    Transaction transaction(db);
    exec(db, "CREATE TABLE IF NOT EXISTS llm_runtime_schema_version("
             "version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL)");
    int version = 0;
    {
        Statement query(db, "SELECT COALESCE(MAX(version),0) FROM llm_runtime_schema_version");
        if(sqlite3_step(query.get()) == SQLITE_ROW) version = sqlite3_column_int(query.get(), 0);
    }
    if(version > 1) throw std::runtime_error("LLM runtime store schema is newer than this binary");
    if(version == 0) {
        exec(db, "CREATE TABLE llm_profiles(tenant_id TEXT NOT NULL,document_id TEXT NOT NULL,"
                 "revision TEXT NOT NULL,digest TEXT NOT NULL,document_json TEXT NOT NULL,"
                 "PRIMARY KEY(tenant_id,document_id,revision))");
        exec(db, "CREATE TABLE llm_prompts(tenant_id TEXT NOT NULL,document_id TEXT NOT NULL,"
                 "revision TEXT NOT NULL,digest TEXT NOT NULL,document_json TEXT NOT NULL,"
                 "PRIMARY KEY(tenant_id,document_id,revision))");
        exec(db, "CREATE TABLE llm_calibrations(tenant_id TEXT NOT NULL,document_id TEXT NOT NULL,"
                 "revision TEXT NOT NULL,digest TEXT NOT NULL,document_json TEXT NOT NULL,"
                 "PRIMARY KEY(tenant_id,document_id,revision))");
        exec(db, "CREATE TABLE llm_invocations(tenant_id TEXT NOT NULL,invocation_id TEXT NOT NULL,"
                 "revision INTEGER NOT NULL,state TEXT NOT NULL,digest TEXT NOT NULL,"
                 "document_json TEXT NOT NULL,updated_at TEXT NOT NULL DEFAULT "
                 "(strftime('%Y-%m-%dT%H:%M:%fZ','now')),PRIMARY KEY(tenant_id,invocation_id))");
        exec(db, "CREATE INDEX llm_invocations_recovery_idx ON llm_invocations(tenant_id,state,updated_at)");
        exec(db, "INSERT INTO llm_runtime_schema_version(version,applied_at) "
                 "VALUES(1,strftime('%Y-%m-%dT%H:%M:%fZ','now'))");
    }
    transaction.commit();
}

RuntimeStoreResult SQLiteLLMRuntimeStore::publish_profile(const LLMRoleProfile& profile) {
    std::lock_guard lock(mutex_);
    if(!validate(profile).empty()) return {RuntimeStoreStatus::Invalid,0,{},"invalid role profile"};
    return publish_document(database(db_), "llm_profiles", profile.metadata.identity.tenant_id,
                            profile.profile_id, profile.revision, profile,
                            [](const auto& value) { return encode(value); });
}

std::optional<LLMRoleProfile> SQLiteLLMRuntimeStore::load_profile(
    std::string_view tenant_id, std::string_view profile_id, std::string_view revision) {
    std::lock_guard lock(mutex_);
    return load_document<LLMRoleProfile>(database(db_), "llm_profiles", tenant_id, profile_id,
        revision, [](const auto& value) { return decode_role_profile(value); });
}

RuntimeStoreResult SQLiteLLMRuntimeStore::publish_prompt(const PromptRevision& prompt) {
    std::lock_guard lock(mutex_);
    if(!validate(prompt).empty()) return {RuntimeStoreStatus::Invalid,0,{},"invalid prompt revision"};
    return publish_document(database(db_), "llm_prompts", prompt.metadata.identity.tenant_id,
                            prompt.prompt_id, prompt.revision, prompt,
                            [](const auto& value) { return encode(value); });
}

std::optional<PromptRevision> SQLiteLLMRuntimeStore::load_prompt(
    std::string_view tenant_id, std::string_view prompt_id, std::string_view revision) {
    std::lock_guard lock(mutex_);
    return load_document<PromptRevision>(database(db_), "llm_prompts", tenant_id, prompt_id,
        revision, [](const auto& value) { return decode_prompt_revision(value); });
}

RuntimeStoreResult SQLiteLLMRuntimeStore::publish_calibration(
    const RoleCalibrationRecord& calibration) {
    std::lock_guard lock(mutex_);
    if(calibration.metadata.identity.tenant_id.empty() || calibration.calibration_id.empty())
        return {RuntimeStoreStatus::Invalid,0,{},"invalid calibration"};
    return publish_document(database(db_), "llm_calibrations", calibration.metadata.identity.tenant_id,
                            calibration.calibration_id, "pinned", calibration,
                            [](const auto& value) { return encode(value); });
}

std::optional<RoleCalibrationRecord> SQLiteLLMRuntimeStore::load_calibration(
    std::string_view tenant_id, std::string_view calibration_id) {
    std::lock_guard lock(mutex_);
    return load_document<RoleCalibrationRecord>(database(db_), "llm_calibrations", tenant_id,
        calibration_id, "pinned", [](const auto& value) { return decode_calibration_record(value); });
}

RuntimeStoreResult SQLiteLLMRuntimeStore::create_invocation(
    const LLMInvocationManifest& manifest) {
    std::lock_guard lock(mutex_);
    if(manifest.state != InvocationState::Pending || !validate(manifest).empty())
        return {RuntimeStoreStatus::Invalid,0,{},"initial invocation must be valid and pending"};
    auto* db = database(db_);
    const auto document = encode(manifest);
    const auto digest = document.at("canonical_digest").get<std::string>();
    Statement insert(db, "INSERT INTO llm_invocations(tenant_id,invocation_id,revision,state,digest,document_json)"
                         " VALUES(?,?,?,?,?,?)");
    bind_text(insert.get(),1,manifest.metadata.identity.tenant_id);
    bind_text(insert.get(),2,manifest.invocation_id);
    sqlite3_bind_int64(insert.get(),3,1);
    bind_text(insert.get(),4,invocation_state_name(manifest.state));
    bind_text(insert.get(),5,digest); bind_text(insert.get(),6,contracts::canonical_json(document));
    const int status=sqlite3_step(insert.get());
    if(status==SQLITE_CONSTRAINT) return {RuntimeStoreStatus::AlreadyExists,0,digest,"invocation exists"};
    if(status!=SQLITE_DONE) return sqlite_failure(db,status);
    return {RuntimeStoreStatus::Committed,1,digest,{}};
}

RuntimeStoreResult SQLiteLLMRuntimeStore::update_invocation(
    const LLMInvocationManifest& manifest, std::uint64_t expected_revision) {
    std::lock_guard lock(mutex_);
    if(!validate(manifest).empty()) return {RuntimeStoreStatus::Invalid,0,{},"invalid invocation manifest"};
    auto* db=database(db_);
    try {
        Transaction transaction(db);
        std::uint64_t actual=0;
        InvocationState prior=InvocationState::Pending;
        {
            Statement query(db,"SELECT revision,state FROM llm_invocations WHERE tenant_id=? AND invocation_id=?");
            bind_text(query.get(),1,manifest.metadata.identity.tenant_id); bind_text(query.get(),2,manifest.invocation_id);
            if(sqlite3_step(query.get())!=SQLITE_ROW)
                return {RuntimeStoreStatus::NotFound,0,{},"invocation not found"};
            actual=static_cast<std::uint64_t>(sqlite3_column_int64(query.get(),0));
            const auto decoded=invocation_state_from_name(column_text(query.get(),1));
            if(!decoded) return {RuntimeStoreStatus::Error,actual,{},"stored invocation state is corrupt"};
            prior=*decoded;
        }
        if(actual!=expected_revision)
            return {RuntimeStoreStatus::RevisionConflict,actual,{},"invocation revision conflict"};
        if(!valid_transition(prior,manifest.state))
            return {RuntimeStoreStatus::Invalid,actual,{},"invalid invocation state transition"};
        const auto document=encode(manifest);
        const auto digest=document.at("canonical_digest").get<std::string>();
        Statement update(db,"UPDATE llm_invocations SET revision=revision+1,state=?,digest=?,document_json=?,"
                            "updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') "
                            "WHERE tenant_id=? AND invocation_id=? AND revision=?");
        bind_text(update.get(),1,invocation_state_name(manifest.state)); bind_text(update.get(),2,digest);
        bind_text(update.get(),3,contracts::canonical_json(document));
        bind_text(update.get(),4,manifest.metadata.identity.tenant_id); bind_text(update.get(),5,manifest.invocation_id);
        sqlite3_bind_int64(update.get(),6,static_cast<sqlite3_int64>(expected_revision));
        const int status=sqlite3_step(update.get());
        if(status!=SQLITE_DONE) return sqlite_failure(db,status);
        if(sqlite3_changes(db)!=1)
            return {RuntimeStoreStatus::RevisionConflict,actual,{},"invocation revision changed concurrently"};
        transaction.commit();
        return {RuntimeStoreStatus::Committed,expected_revision+1,digest,{}};
    } catch(const std::exception& error) {
        return {RuntimeStoreStatus::Error,0,{},error.what()};
    }
}

std::optional<StoredInvocation> SQLiteLLMRuntimeStore::load_invocation(
    std::string_view tenant_id, std::string_view invocation_id) {
    std::lock_guard lock(mutex_);
    auto* db=database(db_);
    Statement query(db,"SELECT revision,document_json,updated_at FROM llm_invocations "
                       "WHERE tenant_id=? AND invocation_id=?");
    bind_text(query.get(),1,tenant_id); bind_text(query.get(),2,invocation_id);
    if(sqlite3_step(query.get())!=SQLITE_ROW) return std::nullopt;
    auto manifest=decode_invocation_manifest(json::parse(column_text(query.get(),1)));
    if(!manifest) throw std::runtime_error("stored invocation manifest is corrupt");
    return StoredInvocation{std::move(*manifest),
        static_cast<std::uint64_t>(sqlite3_column_int64(query.get(),0)),column_text(query.get(),2)};
}

std::vector<StoredInvocation> SQLiteLLMRuntimeStore::list_recoverable(
    std::string_view tenant_id, std::size_t limit) {
    std::lock_guard lock(mutex_);
    std::vector<StoredInvocation> result;
    auto* db=database(db_);
    Statement query(db,"SELECT revision,document_json,updated_at FROM llm_invocations "
                       "WHERE tenant_id=? AND state NOT IN ('succeeded','failed','cancelled','manual_review') "
                       "ORDER BY updated_at LIMIT ?");
    bind_text(query.get(),1,tenant_id);
    sqlite3_bind_int64(query.get(),2,static_cast<sqlite3_int64>(limit));
    while(sqlite3_step(query.get())==SQLITE_ROW) {
        auto manifest=decode_invocation_manifest(json::parse(column_text(query.get(),1)));
        if(!manifest) throw std::runtime_error("stored invocation manifest is corrupt");
        if(!terminal(manifest->state)) result.push_back({std::move(*manifest),
            static_cast<std::uint64_t>(sqlite3_column_int64(query.get(),0)),column_text(query.get(),2)});
    }
    return result;
}

}  // namespace agent_framework::llm_runtime
