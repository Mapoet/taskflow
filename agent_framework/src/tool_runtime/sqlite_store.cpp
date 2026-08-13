#include "agent/tool_runtime/store.hpp"
#include "agent/tool_runtime/state_machine.hpp"
#include "agent/internal/sqlite_utils.hpp"
#include <chrono>
#include <filesystem>
namespace agent_framework::tool_runtime
{
    namespace
    {
        namespace s = internal::sqlite;
        using json = nlohmann::json;
        std::string dig(json v)
        {
            if (v.is_object())
                v.erase("canonical_digest");
            return contracts::canonical_digest(v).value_or("");
        }
        InvocationStoreStatus status(int rc) { return (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) ? InvocationStoreStatus::Busy : InvocationStoreStatus::Error; }
        InvocationEvent event_row(sqlite3_stmt *q)
        {
            InvocationEvent e;
            e.invocation_id = s::column_text(q, 0);
            e.sequence = s::column_uint64(q, 1);
            e.invocation_revision = s::column_uint64(q, 2);
            e.fencing_token = s::column_uint64(q, 3);
            e.event_type = s::column_text(q, 4);
            e.durability = s::column_text(q, 5) == "durable" ? InvocationEventDurability::Durable : InvocationEventDurability::Ephemeral;
            e.payload = json::parse(s::column_text(q, 6));
            e.payload_digest = s::column_text(q, 7);
            e.previous_digest = s::column_text(q, 8);
            e.event_digest = s::column_text(q, 9);
            e.created_at = s::column_text(q, 10);
            e.information_gain = s::column_int(q, 11) != 0;
            return e;
        }
        std::int64_t now_ms()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        }
    }
    SQLiteInvocationStore::SQLiteInvocationStore(std::string path, int busy)
    {
        if (path.empty())
            throw std::invalid_argument("invocation store path required");
        std::filesystem::path p(path);
        if (p.has_parent_path())
            std::filesystem::create_directories(p.parent_path());
        sqlite3 *db = nullptr;
        if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
            throw std::runtime_error(db ? sqlite3_errmsg(db) : "open failed");
        db_ = db;
        sqlite3_busy_timeout(db, busy);
        s::exec(db, "PRAGMA journal_mode=WAL");
        s::exec(db, "PRAGMA synchronous=FULL");
        migrate();
#if !defined(_WIN32)
        std::error_code permission_error;
        std::filesystem::permissions(p, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace,
                                     permission_error);
        if (permission_error)
            throw std::runtime_error("unable to set private invocation store permissions: " + permission_error.message());
#endif
    }
    void SQLiteInvocationStore::migrate()
    {
        auto *db = s::database(db_);
        s::Transaction tx(db);
        s::exec(db, "CREATE TABLE IF NOT EXISTS tool_invocation_schema_version(version INTEGER PRIMARY KEY,applied_at TEXT NOT NULL)");
        int version = 0;
        {
            s::Statement q(db, "SELECT COALESCE(MAX(version),0) FROM tool_invocation_schema_version");
            if (s::step(q.get()) == SQLITE_ROW)
                version = s::column_int(q.get(), 0);
        }
        if (version > 2)
            throw std::runtime_error("invocation store schema is newer than this binary");
        if (version == 0)
        {
            s::exec(db, "CREATE TABLE IF NOT EXISTS tool_invocations(invocation_id TEXT PRIMARY KEY,tenant_id TEXT,conversation_id TEXT,run_id TEXT,tool_call_id TEXT,tool_name TEXT,revision INTEGER,state TEXT,fencing_token INTEGER,document_json TEXT,digest TEXT)");
            s::exec(db, "CREATE INDEX IF NOT EXISTS tool_invocation_scope ON tool_invocations(tenant_id,conversation_id,run_id,tool_call_id)");
            s::exec(db, "CREATE TABLE IF NOT EXISTS tool_invocation_events(invocation_id TEXT,sequence INTEGER,invocation_revision INTEGER,fencing_token INTEGER,event_type TEXT,durability TEXT,payload_json TEXT,payload_digest TEXT,previous_digest TEXT,event_digest TEXT,created_at TEXT,information_gain INTEGER,PRIMARY KEY(invocation_id,sequence))");
            s::exec(db, "CREATE TABLE IF NOT EXISTS tool_progress_checkpoints(invocation_id TEXT,sequence INTEGER,document_json TEXT,digest TEXT,PRIMARY KEY(invocation_id,sequence))");
            s::exec(db, "CREATE TABLE IF NOT EXISTS tool_partial_results(invocation_id TEXT,sequence INTEGER,document_json TEXT,digest TEXT,PRIMARY KEY(invocation_id,sequence))");
            s::exec(db, "CREATE TABLE IF NOT EXISTS tool_invocation_receipts(invocation_id TEXT PRIMARY KEY,document_json TEXT,digest TEXT)");
            s::exec(db, "INSERT INTO tool_invocation_schema_version VALUES(1,strftime('%Y-%m-%dT%H:%M:%fZ','now'))");
            version = 1;
        }
        if (version == 1)
        {
            s::exec(db, "ALTER TABLE tool_invocation_events ADD COLUMN committed_at_ms INTEGER NOT NULL DEFAULT 0");
            s::exec(db, "CREATE TABLE tool_invocation_event_streams(invocation_id TEXT PRIMARY KEY,head_sequence INTEGER NOT NULL,retention_floor INTEGER NOT NULL,last_archive_digest TEXT NOT NULL DEFAULT '',last_archived_event_digest TEXT NOT NULL DEFAULT '',revision INTEGER NOT NULL DEFAULT 0)");
            s::exec(db, "INSERT OR IGNORE INTO tool_invocation_event_streams(invocation_id,head_sequence,retention_floor) SELECT invocation_id,MAX(sequence),MIN(sequence) FROM tool_invocation_events GROUP BY invocation_id");
            s::exec(db, "INSERT INTO tool_invocation_schema_version VALUES(2,strftime('%Y-%m-%dT%H:%M:%fZ','now'))");
        }
        tx.commit();
    }
    SQLiteInvocationStore::~SQLiteInvocationStore()
    {
        if (db_)
            sqlite3_close(s::database(db_));
    }
    StoreResult SQLiteInvocationStore::create(const LongRunningToolInvocation &v)
    {
        std::lock_guard l(mutex_);
        auto issues = validate(v);
        if (!issues.empty())
            return {InvocationStoreStatus::Invalid, 0, issues.front().message};
        auto *j = s::database(db_);
        auto doc = encode(v);
        s::Statement q(j, "INSERT INTO tool_invocations VALUES(?,?,?,?,?,?,?,?,?,?,?)");
        s::bind_text(q.get(), 1, v.invocation_id);
        s::bind_text(q.get(), 2, v.metadata.identity.tenant_id);
        s::bind_text(q.get(), 3, v.conversation_id);
        s::bind_text(q.get(), 4, v.metadata.identity.run_id);
        s::bind_text(q.get(), 5, v.tool_call_id);
        s::bind_text(q.get(), 6, v.tool_name);
        s::bind_uint64(q.get(), 7, v.revision);
        s::bind_text(q.get(), 8, name(v.state));
        s::bind_uint64(q.get(), 9, v.lease.fencing_token);
        s::bind_text(q.get(), 10, doc.dump());
        s::bind_text(q.get(), 11, doc.at("canonical_digest").get<std::string>());
        int rc = s::step(q.get());
        if (rc == SQLITE_CONSTRAINT)
            return {InvocationStoreStatus::AlreadyExists, 0, "invocation exists"};
        return rc == SQLITE_DONE ? StoreResult{InvocationStoreStatus::Committed, v.revision, {}} : StoreResult{status(rc), 0, sqlite3_errmsg(j)};
    }
    std::optional<LongRunningToolInvocation> SQLiteInvocationStore::load(std::string_view id)
    {
        std::lock_guard l(mutex_);
        auto *j = s::database(db_);
        s::Statement q(j, "SELECT document_json,digest FROM tool_invocations WHERE invocation_id=?");
        s::bind_text(q.get(), 1, id);
        if (s::step(q.get()) != SQLITE_ROW)
            return {};
        auto doc = json::parse(s::column_text(q.get(), 0));
        if (doc.value("canonical_digest", "") != s::column_text(q.get(), 1))
            throw std::runtime_error("invocation digest corrupt");
        auto v = decode_invocation(doc);
        if (!v)
            throw std::runtime_error("invocation corrupt");
        return v;
    }
    StoreResult SQLiteInvocationStore::commit(InvocationCommit c)
    {
        std::lock_guard l(mutex_);
        auto *j = s::database(db_);
        try
        {
            s::Transaction tx(j);
            s::Statement old(j, "SELECT revision,state,fencing_token FROM tool_invocations WHERE invocation_id=?");
            s::bind_text(old.get(), 1, c.invocation.invocation_id);
            if (s::step(old.get()) != SQLITE_ROW)
                return {InvocationStoreStatus::NotFound, 0, "not found"};
            auto rev = s::column_uint64(old.get(), 0);
            auto prior = invocation_state(s::column_text(old.get(), 1));
            auto fence = s::column_uint64(old.get(), 2);
            if (rev != c.expected_revision)
                return {InvocationStoreStatus::RevisionConflict, rev, "revision conflict"};
            if (!prior || c.invocation.revision != rev + 1 || !can_transition(*prior, c.invocation.state))
                return {InvocationStoreStatus::Invalid, rev, "illegal transition"};
            const bool fenced_takeover = c.invocation.state == InvocationState::Orphaned &&
                                         c.event.fencing_token > fence;
            if (fence && c.event.fencing_token != fence && !fenced_takeover)
                return {InvocationStoreStatus::FencingRejected, rev, "stale fencing token"};
            if (c.invocation.lease.fencing_token < fence)
                return {InvocationStoreStatus::FencingRejected, rev, "fencing regression"};
            s::Statement tail(j, "SELECT sequence,event_digest FROM tool_invocation_events WHERE invocation_id=? ORDER BY sequence DESC LIMIT 1");
            s::bind_text(tail.get(), 1, c.invocation.invocation_id);
            std::uint64_t seq = 0;
            std::string previous;
            if (s::step(tail.get()) == SQLITE_ROW)
            {
                seq = s::column_uint64(tail.get(), 0);
                previous = s::column_text(tail.get(), 1);
            }
            c.event.invocation_id = c.invocation.invocation_id;
            c.event.sequence = seq + 1;
            c.event.invocation_revision = c.invocation.revision;
            c.event.previous_digest = previous;
            c.event.payload_digest = dig(c.event.payload);
            c.event.event_digest = dig({{"invocation_id", c.event.invocation_id}, {"sequence", c.event.sequence}, {"revision", c.event.invocation_revision}, {"fencing", c.event.fencing_token}, {"type", c.event.event_type}, {"payload_digest", c.event.payload_digest}, {"previous", previous}});
            auto doc = encode(c.invocation);
            s::Statement up(j, "UPDATE tool_invocations SET revision=?,state=?,fencing_token=?,document_json=?,digest=? WHERE invocation_id=? AND revision=?");
            s::bind_uint64(up.get(), 1, c.invocation.revision);
            s::bind_text(up.get(), 2, name(c.invocation.state));
            s::bind_uint64(up.get(), 3, c.invocation.lease.fencing_token);
            s::bind_text(up.get(), 4, doc.dump());
            s::bind_text(up.get(), 5, doc.at("canonical_digest").get<std::string>());
            s::bind_text(up.get(), 6, c.invocation.invocation_id);
            s::bind_uint64(up.get(), 7, rev);
            if (s::step(up.get()) != SQLITE_DONE || s::changes(j) != 1)
                throw std::runtime_error("update conflict");
            s::Statement ev(j, "INSERT INTO tool_invocation_events(invocation_id,sequence,invocation_revision,fencing_token,event_type,durability,payload_json,payload_digest,previous_digest,event_digest,created_at,information_gain,committed_at_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)");
            s::bind_text(ev.get(), 1, c.event.invocation_id);
            s::bind_uint64(ev.get(), 2, c.event.sequence);
            s::bind_uint64(ev.get(), 3, c.event.invocation_revision);
            s::bind_uint64(ev.get(), 4, c.event.fencing_token);
            s::bind_text(ev.get(), 5, c.event.event_type);
            s::bind_text(ev.get(), 6, name(c.event.durability));
            s::bind_text(ev.get(), 7, c.event.payload.dump());
            s::bind_text(ev.get(), 8, c.event.payload_digest);
            s::bind_text(ev.get(), 9, c.event.previous_digest);
            s::bind_text(ev.get(), 10, c.event.event_digest);
            s::bind_text(ev.get(), 11, c.event.created_at);
            s::bind_int(ev.get(), 12, c.event.information_gain);
            s::bind_int64(ev.get(), 13, now_ms());
            if (s::step(ev.get()) != SQLITE_DONE)
                throw std::runtime_error(sqlite3_errmsg(j));
            s::Statement stream(j, "INSERT INTO tool_invocation_event_streams(invocation_id,head_sequence,retention_floor) VALUES(?,?,1) ON CONFLICT(invocation_id) DO UPDATE SET head_sequence=excluded.head_sequence,revision=tool_invocation_event_streams.revision+1");
            s::bind_text(stream.get(), 1, c.invocation.invocation_id);
            s::bind_uint64(stream.get(), 2, c.event.sequence);
            if (s::step(stream.get()) != SQLITE_DONE)
                throw std::runtime_error(sqlite3_errmsg(j));
            auto insert_doc = [&](const char *sql, std::uint64_t sequence, const json &value)
            {s::Statement q(j,sql);s::bind_text(q.get(),1,c.invocation.invocation_id);s::bind_uint64(q.get(),2,sequence);s::bind_text(q.get(),3,value.dump());s::bind_text(q.get(),4,dig(value));if(s::step(q.get())!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(j)); };
            if (c.progress)
                insert_doc("INSERT INTO tool_progress_checkpoints VALUES(?,?,?,?)", c.progress->sequence, encode(*c.progress));
            if (c.partial)
                insert_doc("INSERT INTO tool_partial_results VALUES(?,?,?,?)", c.partial->sequence, encode(*c.partial));
            if (c.receipt)
            {
                auto v = encode(*c.receipt);
                s::Statement q(j, "INSERT INTO tool_invocation_receipts VALUES(?,?,?) ON CONFLICT(invocation_id) DO UPDATE SET document_json=excluded.document_json,digest=excluded.digest");
                s::bind_text(q.get(), 1, c.invocation.invocation_id);
                s::bind_text(q.get(), 2, v.dump());
                s::bind_text(q.get(), 3, dig(v));
                if (s::step(q.get()) != SQLITE_DONE)
                    throw std::runtime_error(sqlite3_errmsg(j));
            }
            tx.commit();
            return {InvocationStoreStatus::Committed, c.invocation.revision, {}};
        }
        catch (const std::exception &e)
        {
            return {InvocationStoreStatus::Error, 0, e.what()};
        }
    }
    std::vector<LongRunningToolInvocation> SQLiteInvocationStore::recoverable(std::size_t limit)
    {
        std::lock_guard l(mutex_);
        auto *j = s::database(db_);
        s::Statement q(j, "SELECT document_json FROM tool_invocations WHERE state NOT IN ('verified','failed','cancelled','manual_review') ORDER BY rowid LIMIT ?");
        s::bind_uint64(q.get(), 1, limit);
        std::vector<LongRunningToolInvocation> o;
        while (s::step(q.get()) == SQLITE_ROW)
        {
            auto v = decode_invocation(json::parse(s::column_text(q.get(), 0)));
            if (!v)
                throw std::runtime_error("recoverable invocation corrupt");
            o.push_back(std::move(*v));
        }
        return o;
    }
    std::vector<InvocationEvent> SQLiteInvocationStore::events(std::string_view id, std::uint64_t after, std::size_t limit)
    {
        std::lock_guard l(mutex_);
        auto *j = s::database(db_);
        s::Statement q(j, "SELECT invocation_id,sequence,invocation_revision,fencing_token,event_type,durability,payload_json,payload_digest,previous_digest,event_digest,created_at,information_gain FROM tool_invocation_events WHERE invocation_id=? AND sequence>? ORDER BY sequence LIMIT ?");
        s::bind_text(q.get(), 1, id);
        s::bind_uint64(q.get(), 2, after);
        s::bind_uint64(q.get(), 3, limit ? limit : static_cast<std::uint64_t>(0x7fffffff));
        std::vector<InvocationEvent> o;
        while (s::step(q.get()) == SQLITE_ROW)
            o.push_back(event_row(q.get()));
        return o;
    }
    std::uint64_t SQLiteInvocationStore::event_head(std::string_view id)
    {
        std::lock_guard l(mutex_);
        auto *j = s::database(db_);
        s::Statement q(j, "SELECT head_sequence FROM tool_invocation_event_streams WHERE invocation_id=?");
        s::bind_text(q.get(), 1, id);
        return s::step(q.get()) == SQLITE_ROW ? s::column_uint64(q.get(), 0) : 0;
    }
    std::uint64_t SQLiteInvocationStore::event_retention_floor(std::string_view id)
    {
        std::lock_guard l(mutex_);
        auto *j = s::database(db_);
        s::Statement q(j, "SELECT retention_floor FROM tool_invocation_event_streams WHERE invocation_id=?");
        s::bind_text(q.get(), 1, id);
        return s::step(q.get()) == SQLITE_ROW ? s::column_uint64(q.get(), 0) : 0;
    }
    std::vector<LongRunningToolInvocation> SQLiteInvocationStore::query(const InvocationQuery &filter)
    {
        std::lock_guard l(mutex_);
        auto *j = s::database(db_);
        s::Statement q(j, "SELECT document_json,digest FROM tool_invocations WHERE (?='' OR tenant_id=?) AND (?='' OR conversation_id=?) AND (?='' OR run_id=?) AND (?='' OR tool_call_id=?) ORDER BY rowid DESC LIMIT ?");
        const std::string values[] = {filter.tenant_id, filter.conversation_id, filter.run_id, filter.tool_call_id};
        int index = 1;
        for (const auto &v : values)
        {
            s::bind_text(q.get(), index++, v);
            s::bind_text(q.get(), index++, v);
        }
        s::bind_uint64(q.get(), index, std::max<std::size_t>(1, filter.limit));
        std::vector<LongRunningToolInvocation> out;
        while (s::step(q.get()) == SQLITE_ROW)
        {
            auto doc = json::parse(s::column_text(q.get(), 0));
            if (doc.value("canonical_digest", "") != s::column_text(q.get(), 1))
                throw std::runtime_error("invocation query digest corrupt");
            auto value = decode_invocation(doc);
            if (!value)
                throw std::runtime_error("invocation query document corrupt");
            out.push_back(std::move(*value));
        }
        return out;
    }
    InvocationRetentionResult SQLiteInvocationStore::apply_retention(std::string_view id,
                                                                     const InvocationRetentionPolicy &policy, distributed::ObjectStore &objects)
    {
        std::lock_guard l(mutex_);
        auto *j = s::database(db_);
        InvocationRetentionResult result;
        try
        {
            std::uint64_t head = 0, floor = 0, revision = 0;
            std::string prior_archive, prior_event, tenant;
            {
                s::Statement q(j, "SELECT head_sequence,retention_floor,last_archive_digest,last_archived_event_digest,revision FROM tool_invocation_event_streams WHERE invocation_id=?");
                s::bind_text(q.get(), 1, id);
                if (s::step(q.get()) != SQLITE_ROW)
                {
                    result.error = "invocation stream not found";
                    return result;
                }
                head = s::column_uint64(q.get(), 0);
                floor = s::column_uint64(q.get(), 1);
                prior_archive = s::column_text(q.get(), 2);
                prior_event = s::column_text(q.get(), 3);
                revision = s::column_uint64(q.get(), 4);
            }
            if (!policy.maximum_events || head <= policy.maximum_events || floor > head - policy.maximum_events)
            {
                result.applied = true;
                return result;
            }
            {
                s::Statement owner(j, "SELECT tenant_id FROM tool_invocations WHERE invocation_id=?");
                s::bind_text(owner.get(), 1, id);
                if (s::step(owner.get()) != SQLITE_ROW)
                {
                    result.error = "invocation not found";
                    return result;
                }
                tenant = s::column_text(owner.get(), 0);
            }
            const auto desired_last = head - policy.maximum_events;
            const auto cutoff = now_ms() - static_cast<std::int64_t>(policy.minimum_age_ms);
            s::Statement q(j, "SELECT invocation_id,sequence,invocation_revision,fencing_token,event_type,durability,payload_json,payload_digest,previous_digest,event_digest,created_at,information_gain FROM tool_invocation_events WHERE invocation_id=? AND sequence>=? AND sequence<=? AND (committed_at_ms=0 OR committed_at_ms<=?) ORDER BY sequence");
            s::bind_text(q.get(), 1, id);
            s::bind_uint64(q.get(), 2, floor);
            s::bind_uint64(q.get(), 3, desired_last);
            s::bind_int64(q.get(), 4, cutoff);
            json archive = {{"schema_version", 1}, {"tenant_id", tenant}, {"invocation_id", std::string(id)}, {"prior_archive_digest", prior_archive}, {"prior_event_digest", prior_event}, {"events", json::array()}};
            std::string last_digest;
            while (s::step(q.get()) == SQLITE_ROW)
            {
                auto e = event_row(q.get());
                if (!result.events)
                    result.first_sequence = e.sequence;
                result.last_sequence = e.sequence;
                last_digest = e.event_digest;
                archive["events"].push_back({{"sequence", e.sequence}, {"invocation_revision", e.invocation_revision}, {"fencing_token", e.fencing_token}, {"event_type", e.event_type}, {"durability", name(e.durability)}, {"payload", e.payload}, {"payload_digest", e.payload_digest}, {"previous_digest", e.previous_digest}, {"event_digest", e.event_digest}, {"created_at", e.created_at}, {"information_gain", e.information_gain}});
                ++result.events;
            }
            if (!result.events)
            {
                result.applied = true;
                return result;
            }
            if (policy.dry_run)
            {
                result.applied = true;
                return result;
            }
            std::string error;
            auto ref = objects.put(tenant, archive.dump(), "application/vnd.taskflow.invocation-events+json", {}, &error);
            if (!ref)
            {
                result.error = "archive failed: " + error;
                return result;
            }
            result.archive_digest = ref->digest;
            s::Transaction tx(j);
            s::Statement del(j, "DELETE FROM tool_invocation_events WHERE invocation_id=? AND sequence>=? AND sequence<=?");
            s::bind_text(del.get(), 1, id);
            s::bind_uint64(del.get(), 2, result.first_sequence);
            s::bind_uint64(del.get(), 3, result.last_sequence);
            if (s::step(del.get()) != SQLITE_DONE)
                throw std::runtime_error(sqlite3_errmsg(j));
            s::Statement up(j, "UPDATE tool_invocation_event_streams SET retention_floor=?,last_archive_digest=?,last_archived_event_digest=?,revision=revision+1 WHERE invocation_id=? AND retention_floor=? AND revision=?");
            s::bind_uint64(up.get(), 1, result.last_sequence + 1);
            s::bind_text(up.get(), 2, result.archive_digest);
            s::bind_text(up.get(), 3, last_digest);
            s::bind_text(up.get(), 4, id);
            s::bind_uint64(up.get(), 5, floor);
            s::bind_uint64(up.get(), 6, revision);
            if (s::step(up.get()) != SQLITE_DONE || s::changes(j) != 1)
                throw std::runtime_error("retention CAS conflict");
            tx.commit();
            result.applied = true;
            return result;
        }
        catch (const std::exception &e)
        {
            result.error = e.what();
            return result;
        }
    }
    std::vector<PartialResultRef> SQLiteInvocationStore::partial_results(std::string_view id)
    {
        std::lock_guard l(mutex_);
        auto *j = s::database(db_);
        s::Statement q(j, "SELECT document_json,digest FROM tool_partial_results WHERE invocation_id=? ORDER BY sequence");
        s::bind_text(q.get(), 1, id);
        std::vector<PartialResultRef> o;
        while (s::step(q.get()) == SQLITE_ROW)
        {
            auto v = json::parse(s::column_text(q.get(), 0));
            if (dig(v) != s::column_text(q.get(), 1))
                throw std::runtime_error("partial result corrupt");
            o.push_back({v.at("sequence"), v.at("kind"), v.at("uri"), v.at("digest"), v.at("media_type"), v.at("size"), v.at("information_gain")});
        }
        return o;
    }
    std::optional<ProgressCheckpoint> SQLiteInvocationStore::latest_progress(std::string_view id)
    {
        std::lock_guard l(mutex_);
        auto *j = s::database(db_);
        s::Statement q(j, "SELECT document_json,digest FROM tool_progress_checkpoints WHERE invocation_id=? ORDER BY sequence DESC LIMIT 1");
        s::bind_text(q.get(), 1, id);
        if (s::step(q.get()) != SQLITE_ROW)
            return {};
        auto v = json::parse(s::column_text(q.get(), 0));
        if (dig(v) != s::column_text(q.get(), 1))
            throw std::runtime_error("progress corrupt");
        return ProgressCheckpoint{v.at("sequence"), v.at("fraction"), v.at("message"), v.at("checkpoint_ref"), v.at("checkpoint_digest"), v.at("updated_at"), v.at("information_gain")};
    }
    HistoryVerification SQLiteInvocationStore::verify_history(std::string_view id)
    {
        auto items = events(id);
        std::string previous;
        std::uint64_t seq = 0;
        {
            std::lock_guard l(mutex_);
            auto *j = s::database(db_);
            s::Statement q(j, "SELECT retention_floor,last_archived_event_digest FROM tool_invocation_event_streams WHERE invocation_id=?");
            s::bind_text(q.get(), 1, id);
            if (s::step(q.get()) == SQLITE_ROW)
            {
                auto floor = s::column_uint64(q.get(), 0);
                if (floor > 1)
                {
                    seq = floor - 1;
                    previous = s::column_text(q.get(), 1);
                    if (previous.empty())
                        return {false, 0, "retention anchor missing"};
                }
            }
        }
        const auto initial = seq;
        for (const auto &e : items)
        {
            if (e.sequence != ++seq || e.previous_digest != previous || e.payload_digest != dig(e.payload))
                return {false, seq - initial - 1, "event chain mismatch"};
            const auto expected = dig({{"invocation_id", e.invocation_id}, {"sequence", e.sequence}, {"revision", e.invocation_revision}, {"fencing", e.fencing_token}, {"type", e.event_type}, {"payload_digest", e.payload_digest}, {"previous", previous}});
            if (expected != e.event_digest)
                return {false, seq - initial - 1, "event digest mismatch"};
            previous = e.event_digest;
        }
        return {true, seq - initial, {}};
    }
}
