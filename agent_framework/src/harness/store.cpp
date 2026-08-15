#include "agent/harness/store.hpp"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

#include <sqlite3.h>

#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::harness {
namespace {
namespace sqlite = internal::sqlite;

bool terminal(HarnessState state) {
    return state == HarnessState::AwaitingExternal ||
           state == HarnessState::Completed || state == HarnessState::Rejected ||
           state == HarnessState::Failed || state == HarnessState::Cancelled ||
           state == HarnessState::ManualReview;
}

bool valid(const HarnessCheckpoint& checkpoint, const HarnessEvent& event,
           std::uint64_t expected_revision, std::string* error) {
    if(checkpoint.metadata.identity.tenant_id.empty() ||
       checkpoint.metadata.identity.task_id.empty() || checkpoint.harness_id.empty() ||
       checkpoint.revision != expected_revision || event.harness_id != checkpoint.harness_id ||
       event.sequence != checkpoint.revision ||
       event.checkpoint_revision != checkpoint.revision || event.event_type.empty()) {
        if(error) *error = "invalid identity, revision, or checkpoint-bound event";
        return false;
    }
    return true;
}

std::string checkpoint_digest(const HarnessCheckpoint& checkpoint) {
    return encode(checkpoint).at("canonical_digest").get<std::string>();
}

HarnessEvent normalized_event(HarnessEvent event) {
    auto digest = contracts::canonical_digest(event.payload);
    if(!digest) throw std::runtime_error("event payload digest failed");
    if(event.payload_digest.empty()) event.payload_digest = *digest;
    if(event.payload_digest != *digest) throw std::invalid_argument("event payload digest mismatch");
    return event;
}

HarnessStoreCommit failure(sqlite3* db, int rc) {
    return {rc == SQLITE_BUSY || rc == SQLITE_LOCKED ? HarnessStoreStatus::Busy
                                                      : HarnessStoreStatus::Error,
            0, {}, sqlite3_errmsg(db)};
}
}  // namespace

std::string InMemoryHarnessStore::key(std::string_view tenant_id,
                                      std::string_view harness_id) {
    return std::string(tenant_id) + '\n' + std::string(harness_id);
}

HarnessStoreCommit InMemoryHarnessStore::create(const HarnessCheckpoint& checkpoint,
                                                 const HarnessEvent& raw_event) {
    std::string error;
    if(!valid(checkpoint, raw_event, 1, &error))
        return {HarnessStoreStatus::Invalid, 0, {}, error};
    HarnessEvent event;
    try { event = normalized_event(raw_event); }
    catch(const std::exception& e) {
        return {HarnessStoreStatus::Invalid, 0, {}, e.what()};
    }
    std::lock_guard lock(mutex_);
    const auto id = key(checkpoint.metadata.identity.tenant_id, checkpoint.harness_id);
    if(checkpoints_.count(id)) return {HarnessStoreStatus::AlreadyExists, 0, {}, {}};
    const auto digest = checkpoint_digest(checkpoint);
    checkpoints_[id] = {checkpoint, checkpoint.revision, digest};
    events_[id].push_back(std::move(event));
    return {HarnessStoreStatus::Committed, checkpoint.revision, digest, {}};
}

std::optional<StoredHarnessCheckpoint> InMemoryHarnessStore::load(
    std::string_view tenant_id, std::string_view harness_id) {
    std::lock_guard lock(mutex_);
    const auto found = checkpoints_.find(key(tenant_id, harness_id));
    return found == checkpoints_.end() ? std::nullopt
                                       : std::optional(found->second);
}

HarnessStoreCommit InMemoryHarnessStore::compare_exchange(
    const HarnessCheckpoint& checkpoint, std::uint64_t expected_revision,
    const HarnessEvent& raw_event) {
    std::string error;
    if(!valid(checkpoint, raw_event, expected_revision + 1, &error))
        return {HarnessStoreStatus::Invalid, 0, {}, error};
    HarnessEvent event;
    try { event = normalized_event(raw_event); }
    catch(const std::exception& e) {
        return {HarnessStoreStatus::Invalid, 0, {}, e.what()};
    }
    std::lock_guard lock(mutex_);
    const auto id = key(checkpoint.metadata.identity.tenant_id, checkpoint.harness_id);
    auto found = checkpoints_.find(id);
    if(found == checkpoints_.end()) return {HarnessStoreStatus::NotFound, 0, {}, {}};
    if(found->second.revision != expected_revision)
        return {HarnessStoreStatus::RevisionConflict, found->second.revision, {}, {}};
    const auto digest = checkpoint_digest(checkpoint);
    found->second = {checkpoint, checkpoint.revision, digest};
    events_[id].push_back(std::move(event));
    return {HarnessStoreStatus::Committed, checkpoint.revision, digest, {}};
}

std::vector<HarnessEvent> InMemoryHarnessStore::events(
    std::string_view tenant_id, std::string_view harness_id, std::uint64_t after_sequence) {
    std::lock_guard lock(mutex_);
    std::vector<HarnessEvent> result;
    const auto found = events_.find(key(tenant_id, harness_id));
    if(found == events_.end()) return result;
    for(const auto& event : found->second)
        if(event.sequence > after_sequence) result.push_back(event);
    return result;
}

std::vector<StoredHarnessCheckpoint> InMemoryHarnessStore::list_recoverable(
    std::string_view tenant_id, std::size_t limit) {
    std::lock_guard lock(mutex_);
    std::vector<StoredHarnessCheckpoint> result;
    for(const auto& [_, value] : checkpoints_) {
        if(value.checkpoint.metadata.identity.tenant_id == tenant_id &&
           !terminal(value.checkpoint.state)) result.push_back(value);
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.checkpoint.updated_at < right.checkpoint.updated_at;
    });
    if(result.size() > limit) result.resize(limit);
    return result;
}

SQLiteHarnessStore::SQLiteHarnessStore(std::string path, SQLiteHarnessStoreOptions options)
    : path_(std::move(path)), options_(options) {
    if(path_.empty()) throw std::invalid_argument("harness store path must not be empty");
    const std::filesystem::path file(path_);
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

SQLiteHarnessStore::~SQLiteHarnessStore() {
    if(db_) sqlite3_close(sqlite::database(db_));
}

void SQLiteHarnessStore::migrate() {
    auto* db = sqlite::database(db_);
    sqlite::exec(db, "CREATE TABLE IF NOT EXISTS phase4_harness_schema_version("
                     "version INTEGER PRIMARY KEY,applied_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)");
    sqlite::exec(db, "INSERT OR IGNORE INTO phase4_harness_schema_version(version) VALUES(1)");
    sqlite::exec(db, "CREATE TABLE IF NOT EXISTS phase4_harness_checkpoints("
                     "tenant_id TEXT NOT NULL,harness_id TEXT NOT NULL,task_id TEXT NOT NULL,"
                     "revision INTEGER NOT NULL,state TEXT NOT NULL,checkpoint_json TEXT NOT NULL,"
                     "checkpoint_digest TEXT NOT NULL,updated_at TEXT NOT NULL,"
                     "PRIMARY KEY(tenant_id,harness_id))");
    sqlite::exec(db, "CREATE TABLE IF NOT EXISTS phase4_harness_events("
                     "tenant_id TEXT NOT NULL,harness_id TEXT NOT NULL,sequence INTEGER NOT NULL,"
                     "checkpoint_revision INTEGER NOT NULL,event_type TEXT NOT NULL,"
                     "payload_json TEXT NOT NULL,payload_digest TEXT NOT NULL,created_at TEXT NOT NULL,"
                     "PRIMARY KEY(tenant_id,harness_id,sequence))");
    sqlite::exec(db, "CREATE INDEX IF NOT EXISTS phase4_harness_recovery_idx ON "
                     "phase4_harness_checkpoints(tenant_id,state,updated_at)");
}

HarnessStoreCommit SQLiteHarnessStore::create(const HarnessCheckpoint& checkpoint,
                                               const HarnessEvent& raw_event) {
    std::string error;
    if(!valid(checkpoint, raw_event, 1, &error))
        return {HarnessStoreStatus::Invalid, 0, {}, error};
    HarnessEvent event;
    try { event = normalized_event(raw_event); }
    catch(const std::exception& e) {
        return {HarnessStoreStatus::Invalid, 0, {}, e.what()};
    }
    const auto document = encode(checkpoint);
    const auto digest = document.at("canonical_digest").get<std::string>();
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    try {
        sqlite::Transaction transaction(db);
        sqlite::Statement insert_checkpoint(db,
            "INSERT INTO phase4_harness_checkpoints(tenant_id,harness_id,task_id,revision,state,"
            "checkpoint_json,checkpoint_digest,updated_at) VALUES(?,?,?,?,?,?,?,?)");
        sqlite::bind_text(insert_checkpoint.get(), 1, checkpoint.metadata.identity.tenant_id);
        sqlite::bind_text(insert_checkpoint.get(), 2, checkpoint.harness_id);
        sqlite::bind_text(insert_checkpoint.get(), 3, checkpoint.metadata.identity.task_id);
        sqlite::bind_int64(insert_checkpoint.get(), 4, checkpoint.revision);
        sqlite::bind_text(insert_checkpoint.get(), 5, harness_state_name(checkpoint.state));
        sqlite::bind_text(insert_checkpoint.get(), 6, document.dump());
        sqlite::bind_text(insert_checkpoint.get(), 7, digest);
        sqlite::bind_text(insert_checkpoint.get(), 8, checkpoint.updated_at);
        const int rc = sqlite::step(insert_checkpoint.get());
        if(rc == SQLITE_CONSTRAINT)
            return {HarnessStoreStatus::AlreadyExists, 0, {}, {}};
        if(rc != SQLITE_DONE) return failure(db, rc);
        sqlite::Statement insert_event(db,
            "INSERT INTO phase4_harness_events(tenant_id,harness_id,sequence,checkpoint_revision,"
            "event_type,payload_json,payload_digest,created_at) VALUES(?,?,?,?,?,?,?,?)");
        sqlite::bind_text(insert_event.get(), 1, checkpoint.metadata.identity.tenant_id);
        sqlite::bind_text(insert_event.get(), 2, checkpoint.harness_id);
        sqlite::bind_int64(insert_event.get(), 3, event.sequence);
        sqlite::bind_int64(insert_event.get(), 4, event.checkpoint_revision);
        sqlite::bind_text(insert_event.get(), 5, event.event_type);
        sqlite::bind_text(insert_event.get(), 6, event.payload.dump());
        sqlite::bind_text(insert_event.get(), 7, event.payload_digest);
        sqlite::bind_text(insert_event.get(), 8, event.created_at);
        if(sqlite::step(insert_event.get()) != SQLITE_DONE) return failure(db, sqlite3_errcode(db));
        transaction.commit();
        return {HarnessStoreStatus::Committed, checkpoint.revision, digest, {}};
    } catch(const std::exception& e) {
        return {HarnessStoreStatus::Error, 0, {}, e.what()};
    }
}

std::optional<StoredHarnessCheckpoint> SQLiteHarnessStore::load(
    std::string_view tenant_id, std::string_view harness_id) {
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    sqlite::Statement statement(db,
        "SELECT revision,checkpoint_json,checkpoint_digest FROM phase4_harness_checkpoints "
        "WHERE tenant_id=? AND harness_id=?");
    sqlite::bind_text(statement.get(), 1, tenant_id);
    sqlite::bind_text(statement.get(), 2, harness_id);
    const int rc = sqlite::step(statement.get());
    if(rc == SQLITE_DONE) return std::nullopt;
    if(rc != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(db));
    const auto revision = static_cast<std::uint64_t>(sqlite::column_int64(statement.get(), 0));
    const auto text = sqlite::column_text(statement.get(), 1);
    const auto digest = sqlite::column_text(statement.get(), 2);
    auto checkpoint = decode_harness_checkpoint(nlohmann::json::parse(text));
    if(!checkpoint || checkpoint->revision != revision || checkpoint_digest(*checkpoint) != digest)
        throw std::runtime_error("stored harness checkpoint is corrupt");
    return StoredHarnessCheckpoint{std::move(*checkpoint), revision, digest};
}

HarnessStoreCommit SQLiteHarnessStore::compare_exchange(
    const HarnessCheckpoint& checkpoint, std::uint64_t expected_revision,
    const HarnessEvent& raw_event) {
    std::string error;
    if(!valid(checkpoint, raw_event, expected_revision + 1, &error))
        return {HarnessStoreStatus::Invalid, 0, {}, error};
    HarnessEvent event;
    try { event = normalized_event(raw_event); }
    catch(const std::exception& e) {
        return {HarnessStoreStatus::Invalid, 0, {}, e.what()};
    }
    const auto document = encode(checkpoint);
    const auto digest = document.at("canonical_digest").get<std::string>();
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    try {
        sqlite::Transaction transaction(db);
        sqlite::Statement update(db,
            "UPDATE phase4_harness_checkpoints SET revision=?,state=?,checkpoint_json=?,"
            "checkpoint_digest=?,updated_at=? WHERE tenant_id=? AND harness_id=? AND revision=?");
        sqlite::bind_int64(update.get(), 1, checkpoint.revision);
        sqlite::bind_text(update.get(), 2, harness_state_name(checkpoint.state));
        sqlite::bind_text(update.get(), 3, document.dump());
        sqlite::bind_text(update.get(), 4, digest);
        sqlite::bind_text(update.get(), 5, checkpoint.updated_at);
        sqlite::bind_text(update.get(), 6, checkpoint.metadata.identity.tenant_id);
        sqlite::bind_text(update.get(), 7, checkpoint.harness_id);
        sqlite::bind_int64(update.get(), 8, expected_revision);
        const int rc = sqlite::step(update.get());
        if(rc != SQLITE_DONE) return failure(db, rc);
        if(sqlite::changes(db) != 1) {
            sqlite::Statement query(db,
                "SELECT revision FROM phase4_harness_checkpoints WHERE tenant_id=? AND harness_id=?");
            sqlite::bind_text(query.get(), 1, checkpoint.metadata.identity.tenant_id);
            sqlite::bind_text(query.get(), 2, checkpoint.harness_id);
            const int qrc = sqlite::step(query.get());
            if(qrc == SQLITE_DONE) return {HarnessStoreStatus::NotFound, 0, {}, {}};
            if(qrc != SQLITE_ROW) return failure(db, qrc);
            return {HarnessStoreStatus::RevisionConflict,
                    static_cast<std::uint64_t>(sqlite::column_int64(query.get(), 0)), {}, {}};
        }
        sqlite::Statement insert_event(db,
            "INSERT INTO phase4_harness_events(tenant_id,harness_id,sequence,checkpoint_revision,"
            "event_type,payload_json,payload_digest,created_at) VALUES(?,?,?,?,?,?,?,?)");
        sqlite::bind_text(insert_event.get(), 1, checkpoint.metadata.identity.tenant_id);
        sqlite::bind_text(insert_event.get(), 2, checkpoint.harness_id);
        sqlite::bind_int64(insert_event.get(), 3, event.sequence);
        sqlite::bind_int64(insert_event.get(), 4, event.checkpoint_revision);
        sqlite::bind_text(insert_event.get(), 5, event.event_type);
        sqlite::bind_text(insert_event.get(), 6, event.payload.dump());
        sqlite::bind_text(insert_event.get(), 7, event.payload_digest);
        sqlite::bind_text(insert_event.get(), 8, event.created_at);
        const int erc = sqlite::step(insert_event.get());
        if(erc != SQLITE_DONE) return failure(db, erc);
        transaction.commit();
        return {HarnessStoreStatus::Committed, checkpoint.revision, digest, {}};
    } catch(const std::exception& e) {
        return {HarnessStoreStatus::Error, 0, {}, e.what()};
    }
}

std::vector<HarnessEvent> SQLiteHarnessStore::events(
    std::string_view tenant_id, std::string_view harness_id, std::uint64_t after_sequence) {
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    sqlite::Statement statement(db,
        "SELECT sequence,checkpoint_revision,event_type,payload_json,payload_digest,created_at "
        "FROM phase4_harness_events WHERE tenant_id=? AND harness_id=? AND sequence>? "
        "ORDER BY sequence");
    sqlite::bind_text(statement.get(), 1, tenant_id);
    sqlite::bind_text(statement.get(), 2, harness_id);
    sqlite::bind_int64(statement.get(), 3, after_sequence);
    std::vector<HarnessEvent> result;
    while(true) {
        const int rc = sqlite::step(statement.get());
        if(rc == SQLITE_DONE) break;
        if(rc != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(db));
        HarnessEvent event;
        event.harness_id = std::string(harness_id);
        event.sequence = static_cast<std::uint64_t>(sqlite::column_int64(statement.get(), 0));
        event.checkpoint_revision =
            static_cast<std::uint64_t>(sqlite::column_int64(statement.get(), 1));
        event.event_type = sqlite::column_text(statement.get(), 2);
        event.payload = nlohmann::json::parse(sqlite::column_text(statement.get(), 3));
        event.payload_digest = sqlite::column_text(statement.get(), 4);
        event.created_at = sqlite::column_text(statement.get(), 5);
        auto digest = contracts::canonical_digest(event.payload);
        if(!digest || *digest != event.payload_digest)
            throw std::runtime_error("stored harness event is corrupt");
        result.push_back(std::move(event));
    }
    return result;
}

std::vector<StoredHarnessCheckpoint> SQLiteHarnessStore::list_recoverable(
    std::string_view tenant_id, std::size_t limit) {
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    sqlite::Statement statement(db,
        "SELECT revision,checkpoint_json,checkpoint_digest FROM phase4_harness_checkpoints "
        "WHERE tenant_id=? AND state IN ('running','awaiting_approval') ORDER BY updated_at LIMIT ?");
    sqlite::bind_text(statement.get(), 1, tenant_id);
    sqlite::bind_int64(statement.get(), 2, static_cast<sqlite3_int64>(limit));
    std::vector<StoredHarnessCheckpoint> result;
    while(true) {
        const int rc = sqlite::step(statement.get());
        if(rc == SQLITE_DONE) break;
        if(rc != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(db));
        const auto revision = static_cast<std::uint64_t>(sqlite::column_int64(statement.get(), 0));
        auto checkpoint = decode_harness_checkpoint(
            nlohmann::json::parse(sqlite::column_text(statement.get(), 1)));
        const auto digest = sqlite::column_text(statement.get(), 2);
        if(!checkpoint || checkpoint->revision != revision || checkpoint_digest(*checkpoint) != digest)
            throw std::runtime_error("stored recoverable harness checkpoint is corrupt");
        result.push_back({std::move(*checkpoint), revision, digest});
    }
    return result;
}

}  // namespace agent_framework::harness
