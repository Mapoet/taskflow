#include "agent/conversation/store.hpp"
#include "agent/internal/sqlite_utils.hpp"
#include "agent/contracts/contract.hpp"
#include "agent/distributed/object_store.hpp"
#include <filesystem>
#include <sqlite3.h>
#include <stdexcept>
namespace agent_framework::conversation
{
    namespace
    {
        using agent_framework::internal::sqlite::Statement;
        std::string digest(nlohmann::json j)
        {
            j.erase("canonical_digest");
            return contracts::canonical_digest(j).value_or("");
        }
        void bind_id(sqlite3_stmt *q, const ConversationIdentity &i)
        {
            internal::sqlite::bind_text(q, 1, i.tenant_id);
            internal::sqlite::bind_text(q, 2, i.conversation_id);
        }
        std::string phase(TurnPhase v) { return std::string(name(v)); }
        TurnPhase parse_phase(std::string_view v)
        {
            for (int i = 0; i < 7; ++i)
                if (name(static_cast<TurnPhase>(i)) == v)
                    return static_cast<TurnPhase>(i);
            throw std::runtime_error("unknown turn phase");
        }
        TurnContinuationReason parse_cont(std::string_view v)
        {
            for (int i = 0; i < 8; ++i)
                if (name(static_cast<TurnContinuationReason>(i)) == v)
                    return static_cast<TurnContinuationReason>(i);
            throw std::runtime_error("unknown continuation");
        }
    }
    SQLiteConversationStore::SQLiteConversationStore(std::string path)
    {
        if (path.empty())
            throw std::invalid_argument("conversation store path required");
        std::filesystem::path p(path);
        std::error_code ec;
        if (p.has_parent_path())
            std::filesystem::create_directories(p.parent_path(), ec);
        sqlite3 *db = nullptr;
        if (ec || sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
        {
            std::string x = db ? sqlite3_errmsg(db) : ec.message();
            if (db)
                sqlite3_close(db);
            throw std::runtime_error(x);
        }
        db_ = db;
        sqlite3_busy_timeout(db, 3000);
        internal::sqlite::exec(db, "PRAGMA journal_mode=WAL");
        internal::sqlite::exec(db, "PRAGMA synchronous=FULL");
        internal::sqlite::exec(db, "PRAGMA foreign_keys=ON");
        internal::sqlite::exec(db, "CREATE TABLE IF NOT EXISTS conversation_messages(tenant TEXT,conversation TEXT,sequence INTEGER,message_id TEXT,parent_id TEXT,turn_id TEXT,role TEXT,content TEXT,created_at TEXT,digest TEXT,PRIMARY KEY(tenant,conversation,sequence),UNIQUE(tenant,conversation,message_id))");
        internal::sqlite::exec(db, "CREATE TABLE IF NOT EXISTS conversation_turns(tenant TEXT,conversation TEXT,turn_id TEXT,revision INTEGER,iteration INTEGER,phase TEXT,continuation TEXT,last_message_id TEXT,boundary_digest TEXT,digest TEXT,PRIMARY KEY(tenant,conversation,turn_id))");
        internal::sqlite::exec(db, "CREATE TABLE IF NOT EXISTS conversation_events(tenant TEXT,conversation TEXT,sequence INTEGER,event_json TEXT,digest TEXT,PRIMARY KEY(tenant,conversation,sequence),UNIQUE(tenant,conversation,event_json))");
        internal::sqlite::exec(db, "CREATE TABLE IF NOT EXISTS conversation_event_streams(tenant TEXT,conversation TEXT,head_sequence INTEGER NOT NULL,retention_floor INTEGER NOT NULL,revision INTEGER NOT NULL DEFAULT 0,last_archive_digest TEXT NOT NULL DEFAULT '',PRIMARY KEY(tenant,conversation))");
        internal::sqlite::exec(db, "INSERT OR IGNORE INTO conversation_event_streams(tenant,conversation,head_sequence,retention_floor,revision,last_archive_digest) SELECT tenant,conversation,MAX(sequence),MIN(sequence),0,'' FROM conversation_events GROUP BY tenant,conversation");
        internal::sqlite::exec(db, "CREATE TABLE IF NOT EXISTS conversation_event_archives(tenant TEXT,conversation TEXT,archive_id TEXT,state TEXT,first_sequence INTEGER,last_sequence INTEGER,event_count INTEGER,first_event_digest TEXT,last_event_digest TEXT,previous_archive_digest TEXT,object_digest TEXT,object_size INTEGER,media_type TEXT,payload_json TEXT,PRIMARY KEY(tenant,conversation,archive_id),UNIQUE(tenant,conversation,first_sequence,last_sequence))");
        internal::sqlite::exec(db, "CREATE TABLE IF NOT EXISTS conversation_boundaries(tenant TEXT,conversation TEXT,revision INTEGER,boundary_json TEXT,digest TEXT,PRIMARY KEY(tenant,conversation,revision))");
        internal::sqlite::exec(db, "CREATE TABLE IF NOT EXISTS conversation_inputs(tenant TEXT,conversation TEXT,sequence INTEGER,input_id TEXT,target_turn_id TEXT,disposition TEXT,state TEXT,input_json TEXT,digest TEXT,PRIMARY KEY(tenant,conversation,sequence),UNIQUE(tenant,conversation,input_id))");
    }
    SQLiteConversationStore::~SQLiteConversationStore()
    {
        if (db_)
            sqlite3_close(internal::sqlite::database(db_));
    }
    bool SQLiteConversationStore::append_message(ConversationMessage m, std::string *e)
    {
        std::lock_guard l(mutex_);
        auto *db = internal::sqlite::database(db_);
        if (m.identity.tenant_id.empty() || m.identity.conversation_id.empty() || m.message_id.empty() || m.sequence == 0)
        {
            if (e)
                *e = "message identity required";
            return false;
        }
        Statement prev(db, "SELECT message_id FROM conversation_messages WHERE tenant=? AND conversation=? ORDER BY sequence DESC LIMIT 1");
        bind_id(prev.get(), m.identity);
        std::string expected;
        if (internal::sqlite::step(prev.get()) == SQLITE_ROW)
            expected = internal::sqlite::column_text(prev.get(), 0);
        if (m.parent_id != expected)
        {
            if (e)
                *e = "message parent chain mismatch";
            return false;
        }
        m.digest = digest(encode(m));
        Statement q(db, "INSERT INTO conversation_messages VALUES(?,?,?,?,?,?,?,?,?,?)");
        bind_id(q.get(), m.identity);
        internal::sqlite::bind_uint64(q.get(), 3, m.sequence);
        internal::sqlite::bind_text(q.get(), 4, m.message_id);
        internal::sqlite::bind_text(q.get(), 5, m.parent_id);
        internal::sqlite::bind_text(q.get(), 6, m.turn_id);
        internal::sqlite::bind_text(q.get(), 7, m.role);
        internal::sqlite::bind_text(q.get(), 8, m.content);
        internal::sqlite::bind_text(q.get(), 9, m.created_at);
        internal::sqlite::bind_text(q.get(), 10, m.digest);
        if (internal::sqlite::step(q.get()) != SQLITE_DONE)
        {
            if (e)
                *e = sqlite3_errmsg(db);
            return false;
        }
        return true;
    }
    std::vector<ConversationMessage> SQLiteConversationStore::messages(const ConversationIdentity &i)
    {
        std::lock_guard l(mutex_);
        auto *db = internal::sqlite::database(db_);
        Statement q(db, "SELECT sequence,message_id,parent_id,turn_id,role,content,created_at,digest FROM conversation_messages WHERE tenant=? AND conversation=? ORDER BY sequence");
        bind_id(q.get(), i);
        std::vector<ConversationMessage> out;
        std::string parent;
        while (internal::sqlite::step(q.get()) == SQLITE_ROW)
        {
            ConversationMessage m;
            m.identity = i;
            m.sequence = internal::sqlite::column_uint64(q.get(), 0);
            m.message_id = internal::sqlite::column_text(q.get(), 1);
            m.parent_id = internal::sqlite::column_text(q.get(), 2);
            m.turn_id = internal::sqlite::column_text(q.get(), 3);
            m.role = internal::sqlite::column_text(q.get(), 4);
            m.content = internal::sqlite::column_text(q.get(), 5);
            m.created_at = internal::sqlite::column_text(q.get(), 6);
            m.digest = internal::sqlite::column_text(q.get(), 7);
            if (m.parent_id != parent || digest(encode(m)) != m.digest)
                return {};
            parent = m.message_id;
            out.push_back(std::move(m));
        }
        return out;
    }
    bool SQLiteConversationStore::commit_turn(TurnCheckpoint c, std::uint64_t expected, std::string *e)
    {
        std::lock_guard l(mutex_);
        auto *db = internal::sqlite::database(db_);
        Statement old(db, "SELECT revision FROM conversation_turns WHERE tenant=? AND conversation=? AND turn_id=?");
        bind_id(old.get(), c.identity);
        internal::sqlite::bind_text(old.get(), 3, c.turn_id);
        std::uint64_t actual = 0;
        if (internal::sqlite::step(old.get()) == SQLITE_ROW)
            actual = internal::sqlite::column_uint64(old.get(), 0);
        if (actual != expected || c.revision != expected + 1)
        {
            if (e)
                *e = "turn revision conflict";
            return false;
        }
        const auto d = digest(encode(c));
        Statement q(db, "INSERT INTO conversation_turns VALUES(?,?,?,?,?,?,?,?,?,?) ON CONFLICT(tenant,conversation,turn_id) DO UPDATE SET revision=excluded.revision,iteration=excluded.iteration,phase=excluded.phase,continuation=excluded.continuation,last_message_id=excluded.last_message_id,boundary_digest=excluded.boundary_digest,digest=excluded.digest");
        bind_id(q.get(), c.identity);
        internal::sqlite::bind_text(q.get(), 3, c.turn_id);
        internal::sqlite::bind_uint64(q.get(), 4, c.revision);
        internal::sqlite::bind_uint64(q.get(), 5, c.iteration);
        internal::sqlite::bind_text(q.get(), 6, phase(c.phase));
        internal::sqlite::bind_text(q.get(), 7, name(c.continuation));
        internal::sqlite::bind_text(q.get(), 8, c.last_message_id);
        internal::sqlite::bind_text(q.get(), 9, c.compact_boundary_digest);
        internal::sqlite::bind_text(q.get(), 10, d);
        if (internal::sqlite::step(q.get()) != SQLITE_DONE)
        {
            if (e)
                *e = sqlite3_errmsg(db);
            return false;
        }
        return true;
    }
    std::optional<TurnCheckpoint> SQLiteConversationStore::load_turn(const ConversationIdentity &i, std::string_view id)
    {
        std::lock_guard l(mutex_);
        auto *db = internal::sqlite::database(db_);
        Statement q(db, "SELECT revision,iteration,phase,continuation,last_message_id,boundary_digest,digest FROM conversation_turns WHERE tenant=? AND conversation=? AND turn_id=?");
        bind_id(q.get(), i);
        internal::sqlite::bind_text(q.get(), 3, id);
        if (internal::sqlite::step(q.get()) != SQLITE_ROW)
            return std::nullopt;
        try
        {
            TurnCheckpoint c;
            c.identity = i;
            c.turn_id = id;
            c.revision = internal::sqlite::column_uint64(q.get(), 0);
            c.iteration = internal::sqlite::column_uint64(q.get(), 1);
            c.phase = parse_phase(internal::sqlite::column_text(q.get(), 2));
            c.continuation = parse_cont(internal::sqlite::column_text(q.get(), 3));
            c.last_message_id = internal::sqlite::column_text(q.get(), 4);
            c.compact_boundary_digest = internal::sqlite::column_text(q.get(), 5);
            if (digest(encode(c)) != internal::sqlite::column_text(q.get(), 6))
                return std::nullopt;
            return c;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }
    std::vector<TurnCheckpoint> SQLiteConversationStore::list_turns(
        const ConversationIdentity &i, bool nonterminal_only, std::size_t limit)
    {
        std::lock_guard l(mutex_);
        auto *db = internal::sqlite::database(db_);
        std::string sql =
            "SELECT turn_id,revision,iteration,phase,continuation,last_message_id,"
            "boundary_digest,digest FROM conversation_turns WHERE tenant=? AND conversation=?";
        if (nonterminal_only)
            sql += " AND phase IN ('pending','running','awaiting_tool','awaiting_input','interrupted')";
        sql += " ORDER BY turn_id";
        if (limit != 0)
            sql += " LIMIT ?";
        Statement q(db, sql.c_str());
        bind_id(q.get(), i);
        if (limit != 0)
            internal::sqlite::bind_uint64(q.get(), 3, limit);
        std::vector<TurnCheckpoint> out;
        while (internal::sqlite::step(q.get()) == SQLITE_ROW)
        {
            TurnCheckpoint c;
            c.identity = i;
            c.turn_id = internal::sqlite::column_text(q.get(), 0);
            c.revision = internal::sqlite::column_uint64(q.get(), 1);
            c.iteration = internal::sqlite::column_uint64(q.get(), 2);
            c.phase = parse_phase(internal::sqlite::column_text(q.get(), 3));
            c.continuation = parse_cont(internal::sqlite::column_text(q.get(), 4));
            c.last_message_id = internal::sqlite::column_text(q.get(), 5);
            c.compact_boundary_digest = internal::sqlite::column_text(q.get(), 6);
            if (digest(encode(c)) != internal::sqlite::column_text(q.get(), 7))
                throw std::runtime_error("stored conversation turn is corrupt");
            out.push_back(std::move(c));
        }
        return out;
    }
    bool SQLiteConversationStore::append_event(RuntimeEventEnvelope v, std::string *e)
    {
        std::lock_guard l(mutex_);
        auto *db = internal::sqlite::database(db_);
        if (v.sequence == 0 || v.event_id.empty())
        {
            if (e)
                *e = "event identity required";
            return false;
        }
        try {
        internal::sqlite::Transaction transaction(db);
        Statement state(db, "SELECT head_sequence FROM conversation_event_streams WHERE tenant=? AND conversation=?");
        internal::sqlite::bind_text(state.get(),1,v.tenant_id); internal::sqlite::bind_text(state.get(),2,v.conversation_id);
        std::uint64_t head=0; if(internal::sqlite::step(state.get())==SQLITE_ROW) head=internal::sqlite::column_uint64(state.get(),0);
        if(v.sequence != head+1) throw std::runtime_error("event sequence must extend durable head");
        auto j = encode(v); v.digest = digest(j);
        Statement q(db, "INSERT INTO conversation_events VALUES(?,?,?,?,?)");
        internal::sqlite::bind_text(q.get(), 1, v.tenant_id);
        internal::sqlite::bind_text(q.get(), 2, v.conversation_id);
        internal::sqlite::bind_uint64(q.get(), 3, v.sequence);
        internal::sqlite::bind_text(q.get(), 4, j.dump());
        internal::sqlite::bind_text(q.get(), 5, v.digest);
        if (internal::sqlite::step(q.get()) != SQLITE_DONE)
        {
            if (e)
                *e = sqlite3_errmsg(db);
            throw std::runtime_error(sqlite3_errmsg(db));
        }
        Statement up(db,"INSERT INTO conversation_event_streams VALUES(?,?,?,?,0,'') ON CONFLICT(tenant,conversation) DO UPDATE SET head_sequence=excluded.head_sequence,retention_floor=CASE WHEN conversation_event_streams.retention_floor=0 THEN excluded.retention_floor ELSE conversation_event_streams.retention_floor END,revision=conversation_event_streams.revision+1");
        internal::sqlite::bind_text(up.get(),1,v.tenant_id); internal::sqlite::bind_text(up.get(),2,v.conversation_id); internal::sqlite::bind_uint64(up.get(),3,v.sequence); internal::sqlite::bind_uint64(up.get(),4,v.sequence);
        if(internal::sqlite::step(up.get())!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
        transaction.commit(); return true;
        } catch(const std::exception& x) { if(e)*e=x.what(); return false; }
    }
    std::vector<RuntimeEventEnvelope> SQLiteConversationStore::events(
        const ConversationIdentity &i, std::uint64_t after, std::size_t limit)
    {
        std::lock_guard l(mutex_);
        const auto stream_key=i.tenant_id+"\x1f"+i.conversation_id;
        event_read_errors_.erase(stream_key);
        auto *db = internal::sqlite::database(db_);
        Statement q(db, limit == 0
            ? "SELECT event_json,digest FROM conversation_events WHERE tenant=? AND conversation=? AND sequence>? ORDER BY sequence"
            : "SELECT event_json,digest FROM conversation_events WHERE tenant=? AND conversation=? AND sequence>? ORDER BY sequence LIMIT ?");
        bind_id(q.get(), i);
        internal::sqlite::bind_uint64(q.get(), 3, after);
        if (limit != 0) internal::sqlite::bind_uint64(q.get(), 4, limit);
        std::vector<RuntimeEventEnvelope> o;
        while (internal::sqlite::step(q.get()) == SQLITE_ROW)
        {
            try
            {
                auto j = nlohmann::json::parse(internal::sqlite::column_text(q.get(), 0));
                if (digest(j) != internal::sqlite::column_text(q.get(), 1))
                    throw std::runtime_error("runtime_event_digest_mismatch");
                std::string decode_error;
                auto decoded=decode_runtime_event(j,&decode_error);
                if(!decoded)throw std::runtime_error(decode_error);
                if(decoded->tenant_id != i.tenant_id ||
                   decoded->conversation_id != i.conversation_id)
                    throw std::runtime_error("runtime_event_scope_mismatch");
                const auto expected=o.empty()?decoded->sequence:o.back().sequence+1;
                if(decoded->sequence!=expected)
                    throw std::runtime_error("runtime_event_reordered_or_duplicate");
                decoded->digest = internal::sqlite::column_text(q.get(), 1);
                o.push_back(std::move(*decoded));
            }
            catch (const std::exception& error)
            {
                event_read_errors_[stream_key]=error.what();
                return {};
            }
        }
        return o;
    }
    std::string SQLiteConversationStore::event_read_error(const ConversationIdentity &i)
    {
        std::lock_guard l(mutex_);
        const auto found=event_read_errors_.find(i.tenant_id+"\x1f"+i.conversation_id);
        return found==event_read_errors_.end()?std::string{}:found->second;
    }
    std::uint64_t SQLiteConversationStore::last_event_sequence(
        const ConversationIdentity &i)
    {
        std::lock_guard l(mutex_);
        auto *db = internal::sqlite::database(db_);
        Statement q(db, "SELECT head_sequence FROM conversation_event_streams WHERE tenant=? AND conversation=?");
        bind_id(q.get(), i);
        return internal::sqlite::step(q.get()) == SQLITE_ROW
            ? internal::sqlite::column_uint64(q.get(), 0) : 0;
    }
    std::uint64_t SQLiteConversationStore::event_retention_floor(
        const ConversationIdentity &i)
    {
        std::lock_guard l(mutex_);
        auto *db = internal::sqlite::database(db_);
        Statement q(db, "SELECT retention_floor FROM conversation_event_streams WHERE tenant=? AND conversation=?");
        bind_id(q.get(), i);
        return internal::sqlite::step(q.get()) == SQLITE_ROW
            ? internal::sqlite::column_uint64(q.get(), 0) : 0;
    }
    std::vector<EventArchiveRecord> SQLiteConversationStore::event_archives(
        const ConversationIdentity &i)
    {
        std::lock_guard l(mutex_); auto *db=internal::sqlite::database(db_);
        Statement q(db,"SELECT archive_id,state,first_sequence,last_sequence,event_count,first_event_digest,last_event_digest,previous_archive_digest,object_digest,object_size,media_type FROM conversation_event_archives WHERE tenant=? AND conversation=? ORDER BY first_sequence");
        bind_id(q.get(),i); std::vector<EventArchiveRecord> out;
        while(internal::sqlite::step(q.get())==SQLITE_ROW) {
            EventArchiveRecord r; r.identity=i; r.archive_id=internal::sqlite::column_text(q.get(),0);
            r.state=internal::sqlite::column_text(q.get(),1); r.first_sequence=internal::sqlite::column_uint64(q.get(),2);
            r.last_sequence=internal::sqlite::column_uint64(q.get(),3); r.event_count=internal::sqlite::column_uint64(q.get(),4);
            r.first_event_digest=internal::sqlite::column_text(q.get(),5); r.last_event_digest=internal::sqlite::column_text(q.get(),6);
            r.previous_archive_digest=internal::sqlite::column_text(q.get(),7); r.object_digest=internal::sqlite::column_text(q.get(),8);
            r.object_size=internal::sqlite::column_uint64(q.get(),9); r.media_type=internal::sqlite::column_text(q.get(),10);
            out.push_back(std::move(r));
        } return out;
    }
    bool SQLiteConversationStore::verify_event_archive(const EventArchiveRecord &r,
        distributed::ObjectStore &objects, std::string *e)
    {
        distributed::ObjectRef ref{r.identity.tenant_id,r.object_digest,r.object_size,r.media_type};
        auto bytes=objects.get(ref,e); if(!bytes) return false;
        try {
            auto root=nlohmann::json::parse(*bytes);
            const auto &m=root.at("manifest"); const auto &items=root.at("events");
            if(m.at("schema")!="agent.conversation_event_archive/v1" ||
               m.at("tenant_id")!=r.identity.tenant_id || m.at("conversation_id")!=r.identity.conversation_id ||
               m.at("first_sequence").get<std::uint64_t>()!=r.first_sequence ||
               m.at("last_sequence").get<std::uint64_t>()!=r.last_sequence ||
               m.at("event_count").get<std::uint64_t>()!=r.event_count ||
               m.at("previous_archive_digest")!=r.previous_archive_digest || !items.is_array() || items.size()!=r.event_count)
                throw std::runtime_error("archive manifest mismatch");
            std::uint64_t sequence=r.first_sequence; std::string first,last;
            for(const auto &item:items) {
                auto event=decode_runtime_event(item); if(!event || event->sequence!=sequence++)
                    throw std::runtime_error("archive event integrity or sequence mismatch");
                const auto d=digest(item); if(first.empty()) first=d; last=d;
            }
            if(first!=r.first_event_digest || last!=r.last_event_digest)
                throw std::runtime_error("archive digest boundary mismatch");
            return true;
        } catch(const std::exception &x) { if(e)*e=x.what(); return false; }
    }
    EventCompactionResult SQLiteConversationStore::compact_events(const ConversationIdentity &i,
        const EventRetentionPolicy &policy, distributed::ObjectStore &objects)
    {
        EventCompactionResult result; result.dry_run=policy.dry_run;
        if(i.tenant_id.empty()||i.conversation_id.empty()||policy.maximum_archive_events==0) {
            result.error="invalid event compaction policy or identity"; return result;
        }
        std::lock_guard l(mutex_); auto *db=internal::sqlite::database(db_);
        try {
            std::string archive_id,payload,previous,first_digest,last_digest;
            std::uint64_t first=0,last=0,count=0;
            {
                Statement pending(db,"SELECT archive_id,first_sequence,last_sequence,event_count,first_event_digest,last_event_digest,previous_archive_digest,payload_json FROM conversation_event_archives WHERE tenant=? AND conversation=? AND state='prepared' ORDER BY first_sequence LIMIT 1");
                bind_id(pending.get(),i);
                if(internal::sqlite::step(pending.get())==SQLITE_ROW) {
                    archive_id=internal::sqlite::column_text(pending.get(),0); first=internal::sqlite::column_uint64(pending.get(),1);
                    last=internal::sqlite::column_uint64(pending.get(),2); count=internal::sqlite::column_uint64(pending.get(),3);
                    first_digest=internal::sqlite::column_text(pending.get(),4); last_digest=internal::sqlite::column_text(pending.get(),5);
                    previous=internal::sqlite::column_text(pending.get(),6); payload=internal::sqlite::column_text(pending.get(),7);
                }
            }
            if(archive_id.empty()) {
                Statement state(db,"SELECT head_sequence,retention_floor,last_archive_digest FROM conversation_event_streams WHERE tenant=? AND conversation=?"); bind_id(state.get(),i);
                if(internal::sqlite::step(state.get())!=SQLITE_ROW) { result.ok=true; return result; }
                const auto head=internal::sqlite::column_uint64(state.get(),0); const auto floor=internal::sqlite::column_uint64(state.get(),1); previous=internal::sqlite::column_text(state.get(),2);
                if(head<=policy.keep_last_events || floor==0) { result.ok=true; return result; }
                const auto eligible=head-static_cast<std::uint64_t>(policy.keep_last_events);
                last=std::min(eligible,floor+static_cast<std::uint64_t>(policy.maximum_archive_events)-1);
                Statement items(db,"SELECT event_json,digest,sequence FROM conversation_events WHERE tenant=? AND conversation=? AND sequence>=? AND sequence<=? ORDER BY sequence");
                bind_id(items.get(),i); internal::sqlite::bind_uint64(items.get(),3,floor); internal::sqlite::bind_uint64(items.get(),4,last);
                nlohmann::json events=nlohmann::json::array(); std::uint64_t expected=floor;
                while(internal::sqlite::step(items.get())==SQLITE_ROW) {
                    const auto sequence=internal::sqlite::column_uint64(items.get(),2); if(sequence!=expected++) throw std::runtime_error("online event range has a gap");
                    auto item=nlohmann::json::parse(internal::sqlite::column_text(items.get(),0)); const auto stored=internal::sqlite::column_text(items.get(),1);
                    if(digest(item)!=stored) throw std::runtime_error("online event integrity failure");
                    if(first_digest.empty()) first_digest=stored;
                    last_digest=stored;
                    events.push_back(std::move(item));
                }
                count=events.size(); if(count==0) { result.ok=true; return result; }
                first=floor; last=first+count-1; archive_id=std::to_string(first)+"-"+std::to_string(last);
                nlohmann::json manifest={{"schema","agent.conversation_event_archive/v1"},{"tenant_id",i.tenant_id},{"conversation_id",i.conversation_id},{"first_sequence",first},{"last_sequence",last},{"event_count",count},{"first_event_digest",first_digest},{"last_event_digest",last_digest},{"previous_archive_digest",previous}};
                payload=nlohmann::json{{"manifest",manifest},{"events",events}}.dump();
                result.first_sequence=first; result.last_sequence=last; result.event_count=count; result.archive_id=archive_id;
                if(policy.dry_run) { result.ok=true; result.changed=true; return result; }
                internal::sqlite::Transaction tx(db);
                Statement prepare(db,"INSERT INTO conversation_event_archives VALUES(?,?,?,'prepared',?,?,?,?,?,?, '',0,'application/vnd.taskflow.conversation-events+json',?)");
                bind_id(prepare.get(),i); internal::sqlite::bind_text(prepare.get(),3,archive_id); internal::sqlite::bind_uint64(prepare.get(),4,first); internal::sqlite::bind_uint64(prepare.get(),5,last); internal::sqlite::bind_uint64(prepare.get(),6,count); internal::sqlite::bind_text(prepare.get(),7,first_digest); internal::sqlite::bind_text(prepare.get(),8,last_digest); internal::sqlite::bind_text(prepare.get(),9,previous); internal::sqlite::bind_text(prepare.get(),10,payload);
                if(internal::sqlite::step(prepare.get())!=SQLITE_DONE)
                    throw std::runtime_error(sqlite3_errmsg(db));
                tx.commit();
            }
            result.first_sequence=first; result.last_sequence=last; result.event_count=count; result.archive_id=archive_id;
            std::string object_error; auto ref=objects.put(i.tenant_id,payload,"application/vnd.taskflow.conversation-events+json",{},&object_error);
            if(!ref) throw std::runtime_error("archive upload failed: "+object_error);
            EventArchiveRecord record{i,archive_id,"prepared",ref->digest,"application/vnd.taskflow.conversation-events+json",first,last,count,ref->size,first_digest,last_digest,previous};
            if(!verify_event_archive(record,objects,&object_error)) throw std::runtime_error("archive verification failed: "+object_error);
            internal::sqlite::Transaction tx(db);
            Statement check(db,"SELECT COUNT(*),MIN(sequence),MAX(sequence) FROM conversation_events WHERE tenant=? AND conversation=? AND sequence>=? AND sequence<=?"); bind_id(check.get(),i); internal::sqlite::bind_uint64(check.get(),3,first); internal::sqlite::bind_uint64(check.get(),4,last);
            if(internal::sqlite::step(check.get())!=SQLITE_ROW || internal::sqlite::column_uint64(check.get(),0)!=count || internal::sqlite::column_uint64(check.get(),1)!=first || internal::sqlite::column_uint64(check.get(),2)!=last) throw std::runtime_error("archive source range changed");
            Statement erase(db,"DELETE FROM conversation_events WHERE tenant=? AND conversation=? AND sequence>=? AND sequence<=?"); bind_id(erase.get(),i); internal::sqlite::bind_uint64(erase.get(),3,first); internal::sqlite::bind_uint64(erase.get(),4,last);
            if(internal::sqlite::step(erase.get())!=SQLITE_DONE || static_cast<std::uint64_t>(internal::sqlite::changes(db))!=count) throw std::runtime_error("archive prune conflict");
            Statement commit(db,"UPDATE conversation_event_archives SET state='pruned',object_digest=?,object_size=?,payload_json='' WHERE tenant=? AND conversation=? AND archive_id=? AND state='prepared'"); internal::sqlite::bind_text(commit.get(),1,ref->digest); internal::sqlite::bind_uint64(commit.get(),2,ref->size); internal::sqlite::bind_text(commit.get(),3,i.tenant_id); internal::sqlite::bind_text(commit.get(),4,i.conversation_id); internal::sqlite::bind_text(commit.get(),5,archive_id);
            if(internal::sqlite::step(commit.get())!=SQLITE_DONE || internal::sqlite::changes(db)!=1) throw std::runtime_error("archive state conflict");
            Statement stream(db,"UPDATE conversation_event_streams SET retention_floor=?,revision=revision+1,last_archive_digest=? WHERE tenant=? AND conversation=? AND retention_floor=?"); internal::sqlite::bind_uint64(stream.get(),1,last+1); internal::sqlite::bind_text(stream.get(),2,ref->digest); internal::sqlite::bind_text(stream.get(),3,i.tenant_id); internal::sqlite::bind_text(stream.get(),4,i.conversation_id); internal::sqlite::bind_uint64(stream.get(),5,first);
            if(internal::sqlite::step(stream.get())!=SQLITE_DONE || internal::sqlite::changes(db)!=1) throw std::runtime_error("retention floor conflict");
            tx.commit(); result.ok=true; result.changed=true; result.object_digest=ref->digest; return result;
        } catch(const std::exception &x) { result.error=x.what(); return result; }
    }
    bool SQLiteConversationStore::append_boundary(CompactBoundaryRecord v, std::string *e)
    {
        std::lock_guard l(mutex_);
        auto *db = internal::sqlite::database(db_);
        if (v.revision == 0 || v.boundary_id.empty() || v.summary_digest.empty())
        {
            if (e)
                *e = "boundary identity required";
            return false;
        }
        auto j = encode(v);
        v.digest = digest(j);
        Statement q(db, "INSERT INTO conversation_boundaries VALUES(?,?,?,?,?)");
        bind_id(q.get(), v.identity);
        internal::sqlite::bind_uint64(q.get(), 3, v.revision);
        internal::sqlite::bind_text(q.get(), 4, j.dump());
        internal::sqlite::bind_text(q.get(), 5, v.digest);
        if (internal::sqlite::step(q.get()) != SQLITE_DONE)
        {
            if (e)
                *e = sqlite3_errmsg(db);
            return false;
        }
        return true;
    }
    std::optional<CompactBoundaryRecord> SQLiteConversationStore::latest_boundary(const ConversationIdentity &i)
    {
        std::lock_guard l(mutex_);
        auto *db = internal::sqlite::database(db_);
        Statement q(db, "SELECT boundary_json,digest FROM conversation_boundaries WHERE tenant=? AND conversation=? ORDER BY revision DESC LIMIT 1");
        bind_id(q.get(), i);
        if (internal::sqlite::step(q.get()) != SQLITE_ROW)
            return std::nullopt;
        try
        {
            auto j = nlohmann::json::parse(internal::sqlite::column_text(q.get(), 0));
            if (digest(j) != internal::sqlite::column_text(q.get(), 1))
                return std::nullopt;
            CompactBoundaryRecord v;
            v.identity = i;
            v.boundary_id = j.at("boundary_id");
            v.turn_id = j.at("turn_id");
            v.revision = j.at("revision");
            v.summary_ref = j.at("summary_ref");
            v.summary_digest = j.at("summary_digest");
            v.pre_tokens = j.at("pre_tokens");
            v.post_tokens = j.at("post_tokens");
            v.archived_message_ids =
                j.at("archived_message_ids").get<std::vector<std::string>>();
            v.preserved_message_ids =
                j.at("preserved_message_ids").get<std::vector<std::string>>();
            v.profile_revision_digest = j.at("profile_revision_digest");
            v.prompt_revision_digest = j.at("prompt_revision_digest");
            v.model = j.at("model");
            v.fallback_reason = j.at("fallback_reason");
            v.digest = internal::sqlite::column_text(q.get(), 1);
            return v;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }
    bool SQLiteConversationStore::commit(ConversationCommit &batch, std::string *e)
    {
        std::lock_guard l(mutex_);
        auto *db = internal::sqlite::database(db_);
        try
        {
            internal::sqlite::Transaction transaction(db);
            const auto &identity = batch.checkpoint.identity;
            if (identity.tenant_id.empty() || identity.conversation_id.empty() ||
                batch.checkpoint.turn_id.empty())
                throw std::runtime_error("conversation commit identity required");

            Statement old(db, "SELECT revision FROM conversation_turns WHERE tenant=? AND conversation=? AND turn_id=?");
            bind_id(old.get(), identity);
            internal::sqlite::bind_text(old.get(), 3, batch.checkpoint.turn_id);
            std::uint64_t actual = 0;
            if (internal::sqlite::step(old.get()) == SQLITE_ROW)
                actual = internal::sqlite::column_uint64(old.get(), 0);
            if (actual != batch.expected_turn_revision ||
                batch.checkpoint.revision != batch.expected_turn_revision + 1)
                throw std::runtime_error("turn revision conflict");

            Statement tail(db, "SELECT sequence,message_id FROM conversation_messages WHERE tenant=? AND conversation=? ORDER BY sequence DESC LIMIT 1");
            bind_id(tail.get(), identity);
            std::uint64_t message_sequence = 0;
            std::string parent;
            if (internal::sqlite::step(tail.get()) == SQLITE_ROW)
            {
                message_sequence = internal::sqlite::column_uint64(tail.get(), 0);
                parent = internal::sqlite::column_text(tail.get(), 1);
            }
            for (auto &message : batch.messages)
            {
                if (message.identity.tenant_id != identity.tenant_id ||
                    message.identity.conversation_id != identity.conversation_id ||
                    message.parent_id != parent || message.message_id.empty())
                    throw std::runtime_error("message parent chain mismatch");
                message.sequence = ++message_sequence;
                message.digest = digest(encode(message));
                Statement insert(db, "INSERT INTO conversation_messages VALUES(?,?,?,?,?,?,?,?,?,?)");
                bind_id(insert.get(), identity);
                internal::sqlite::bind_uint64(insert.get(), 3, message.sequence);
                internal::sqlite::bind_text(insert.get(), 4, message.message_id);
                internal::sqlite::bind_text(insert.get(), 5, message.parent_id);
                internal::sqlite::bind_text(insert.get(), 6, message.turn_id);
                internal::sqlite::bind_text(insert.get(), 7, message.role);
                internal::sqlite::bind_text(insert.get(), 8, message.content);
                internal::sqlite::bind_text(insert.get(), 9, message.created_at);
                internal::sqlite::bind_text(insert.get(), 10, message.digest);
                if (internal::sqlite::step(insert.get()) != SQLITE_DONE)
                    throw std::runtime_error(sqlite3_errmsg(db));
                parent = message.message_id;
            }
            if (!batch.messages.empty())
                batch.checkpoint.last_message_id = batch.messages.back().message_id;

            Statement event_tail(db, "SELECT head_sequence FROM conversation_event_streams WHERE tenant=? AND conversation=?");
            bind_id(event_tail.get(), identity);
            std::uint64_t event_sequence = internal::sqlite::step(event_tail.get()) == SQLITE_ROW
                ? internal::sqlite::column_uint64(event_tail.get(), 0) : 0;
            for (auto &event : batch.durable_events)
            {
                event.tenant_id = identity.tenant_id;
                event.conversation_id = identity.conversation_id;
                event.sequence = ++event_sequence;
                if (event.event_id.empty())
                    event.event_id = event.turn_id + ":" + std::to_string(event.sequence);
                const auto json = encode(event);
                event.digest = digest(json);
                Statement insert(db, "INSERT INTO conversation_events VALUES(?,?,?,?,?)");
                bind_id(insert.get(), identity);
                internal::sqlite::bind_uint64(insert.get(), 3, event.sequence);
                internal::sqlite::bind_text(insert.get(), 4, json.dump());
                internal::sqlite::bind_text(insert.get(), 5, event.digest);
                if (internal::sqlite::step(insert.get()) != SQLITE_DONE)
                    throw std::runtime_error(sqlite3_errmsg(db));
            }
            if (!batch.durable_events.empty()) {
                Statement up(db,"INSERT INTO conversation_event_streams VALUES(?,?,?,?,0,'') ON CONFLICT(tenant,conversation) DO UPDATE SET head_sequence=excluded.head_sequence,retention_floor=CASE WHEN conversation_event_streams.retention_floor=0 THEN excluded.retention_floor ELSE conversation_event_streams.retention_floor END,revision=conversation_event_streams.revision+1");
                bind_id(up.get(),identity); internal::sqlite::bind_uint64(up.get(),3,event_sequence); internal::sqlite::bind_uint64(up.get(),4,batch.durable_events.front().sequence);
                if(internal::sqlite::step(up.get())!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
            }

            Statement input_tail(db, "SELECT COALESCE(MAX(sequence),0) FROM conversation_inputs WHERE tenant=? AND conversation=?");
            bind_id(input_tail.get(), identity);
            std::uint64_t input_sequence = internal::sqlite::step(input_tail.get()) == SQLITE_ROW
                ? internal::sqlite::column_uint64(input_tail.get(), 0) : 0;
            for (auto &input : batch.inputs)
            {
                if (input.identity.tenant_id != identity.tenant_id ||
                    input.identity.conversation_id != identity.conversation_id ||
                    input.input_id.empty() || input.content.empty())
                    throw std::runtime_error("conversation input identity required");
                input.sequence = ++input_sequence;
                const auto json = encode(input);
                input.digest = digest(json);
                Statement insert(db, "INSERT INTO conversation_inputs VALUES(?,?,?,?,?,?,?,?,?)");
                bind_id(insert.get(), identity);
                internal::sqlite::bind_uint64(insert.get(), 3, input.sequence);
                internal::sqlite::bind_text(insert.get(), 4, input.input_id);
                internal::sqlite::bind_text(insert.get(), 5, input.target_turn_id);
                internal::sqlite::bind_text(insert.get(), 6, name(input.disposition));
                internal::sqlite::bind_text(insert.get(), 7, name(input.state));
                internal::sqlite::bind_text(insert.get(), 8, json.dump());
                internal::sqlite::bind_text(insert.get(), 9, input.digest);
                if (internal::sqlite::step(insert.get()) != SQLITE_DONE)
                    throw std::runtime_error(sqlite3_errmsg(db));
            }

            const auto checkpoint_digest = digest(encode(batch.checkpoint));
            Statement turn(db, "INSERT INTO conversation_turns VALUES(?,?,?,?,?,?,?,?,?,?) ON CONFLICT(tenant,conversation,turn_id) DO UPDATE SET revision=excluded.revision,iteration=excluded.iteration,phase=excluded.phase,continuation=excluded.continuation,last_message_id=excluded.last_message_id,boundary_digest=excluded.boundary_digest,digest=excluded.digest");
            bind_id(turn.get(), identity);
            internal::sqlite::bind_text(turn.get(), 3, batch.checkpoint.turn_id);
            internal::sqlite::bind_uint64(turn.get(), 4, batch.checkpoint.revision);
            internal::sqlite::bind_uint64(turn.get(), 5, batch.checkpoint.iteration);
            internal::sqlite::bind_text(turn.get(), 6, phase(batch.checkpoint.phase));
            internal::sqlite::bind_text(turn.get(), 7, name(batch.checkpoint.continuation));
            internal::sqlite::bind_text(turn.get(), 8, batch.checkpoint.last_message_id);
            internal::sqlite::bind_text(turn.get(), 9, batch.checkpoint.compact_boundary_digest);
            internal::sqlite::bind_text(turn.get(), 10, checkpoint_digest);
            if (internal::sqlite::step(turn.get()) != SQLITE_DONE)
                throw std::runtime_error(sqlite3_errmsg(db));
            transaction.commit();
            return true;
        }
        catch (const std::exception &failure)
        {
            if (e)
                *e = failure.what();
            return false;
        }
    }
    std::vector<ConversationInput> SQLiteConversationStore::inputs(
        const ConversationIdentity &identity, InputState state)
    {
        std::lock_guard l(mutex_);
        auto *db = internal::sqlite::database(db_);
        Statement query(db, "SELECT input_json,digest FROM conversation_inputs WHERE tenant=? AND conversation=? AND state=? ORDER BY sequence");
        bind_id(query.get(), identity);
        internal::sqlite::bind_text(query.get(), 3, name(state));
        std::vector<ConversationInput> result;
        while (internal::sqlite::step(query.get()) == SQLITE_ROW)
        {
            try
            {
                auto json = nlohmann::json::parse(internal::sqlite::column_text(query.get(), 0));
                if (digest(json) != internal::sqlite::column_text(query.get(), 1))
                    return {};
                ConversationInput input;
                input.identity = identity;
                input.input_id = json.at("input_id");
                input.target_turn_id = json.at("target_turn_id");
                input.consumed_turn_id = json.value("consumed_turn_id", "");
                input.content = json.at("content");
                input.created_at = json.at("created_at");
                input.sequence = json.at("sequence");
                if (auto profile = task_execution_profile(json.value("profile", "conversation")))
                    input.profile = *profile;
                input.max_iterations = json.value("max_iterations", 10ULL);
                input.max_input_tokens = json.value("max_input_tokens", 0ULL);
                input.max_output_tokens = json.value("max_output_tokens", 0ULL);
                const auto disposition = json.at("disposition").get<std::string>();
                for (int i = 0; i < 5; ++i)
                    if (name(static_cast<InputDisposition>(i)) == disposition)
                        input.disposition = static_cast<InputDisposition>(i);
                input.state = state;
                input.digest = internal::sqlite::column_text(query.get(), 1);
                result.push_back(std::move(input));
            }
            catch (...)
            {
                return {};
            }
        }
        return result;
    }
    std::optional<QueuedTurnClaim> SQLiteConversationStore::consume_next_queued_input(
        const ConversationIdentity &identity, std::string *e)
    {
        std::lock_guard l(mutex_);
        auto *db = internal::sqlite::database(db_);
        try
        {
            internal::sqlite::Transaction transaction(db);
            Statement queued(db, "SELECT sequence,input_json,digest FROM conversation_inputs WHERE tenant=? AND conversation=? AND state='queued' AND disposition IN ('queue_next_turn','interrupt_and_replace') ORDER BY sequence LIMIT 1");
            bind_id(queued.get(), identity);
            if (internal::sqlite::step(queued.get()) != SQLITE_ROW)
            {
                transaction.commit();
                return std::nullopt;
            }
            const auto input_sequence = internal::sqlite::column_uint64(queued.get(), 0);
            auto json = nlohmann::json::parse(internal::sqlite::column_text(queued.get(), 1));
            if (digest(json) != internal::sqlite::column_text(queued.get(), 2))
                throw std::runtime_error("conversation input digest mismatch");

            ConversationInput input;
            input.identity = identity;
            input.input_id = json.at("input_id");
            input.target_turn_id = json.at("target_turn_id");
            input.content = json.at("content");
            input.created_at = json.at("created_at");
            input.sequence = input_sequence;
            if (auto profile = task_execution_profile(json.value("profile", "conversation")))
                input.profile = *profile;
            input.max_iterations = json.value("max_iterations", 10ULL);
            input.max_input_tokens = json.value("max_input_tokens", 0ULL);
            input.max_output_tokens = json.value("max_output_tokens", 0ULL);
            const auto disposition = json.at("disposition").get<std::string>();
            input.disposition = disposition == "interrupt_and_replace"
                ? InputDisposition::InterruptAndReplace : InputDisposition::QueueNextTurn;
            input.state = InputState::Consumed;
            input.consumed_turn_id = input.input_id + ":turn";

            Statement existing(db, "SELECT phase FROM conversation_turns WHERE tenant=? AND conversation=? AND turn_id=?");
            bind_id(existing.get(), identity);
            internal::sqlite::bind_text(existing.get(), 3, input.consumed_turn_id);
            if (internal::sqlite::step(existing.get()) == SQLITE_ROW)
                throw std::runtime_error("queued input already materialized");

            Statement tail(db, "SELECT sequence,message_id FROM conversation_messages WHERE tenant=? AND conversation=? ORDER BY sequence DESC LIMIT 1");
            bind_id(tail.get(), identity);
            std::uint64_t message_sequence = 0;
            std::string parent;
            if (internal::sqlite::step(tail.get()) == SQLITE_ROW)
            {
                message_sequence = internal::sqlite::column_uint64(tail.get(), 0);
                parent = internal::sqlite::column_text(tail.get(), 1);
            }
            ConversationMessage message;
            message.identity = identity;
            message.message_id = input.consumed_turn_id + ":user";
            message.parent_id = parent;
            message.turn_id = input.consumed_turn_id;
            message.role = "user";
            message.content = input.content;
            message.created_at = input.created_at;
            message.sequence = ++message_sequence;
            message.digest = digest(encode(message));
            Statement insert_message(db, "INSERT INTO conversation_messages VALUES(?,?,?,?,?,?,?,?,?,?)");
            bind_id(insert_message.get(), identity);
            internal::sqlite::bind_uint64(insert_message.get(), 3, message.sequence);
            internal::sqlite::bind_text(insert_message.get(), 4, message.message_id);
            internal::sqlite::bind_text(insert_message.get(), 5, message.parent_id);
            internal::sqlite::bind_text(insert_message.get(), 6, message.turn_id);
            internal::sqlite::bind_text(insert_message.get(), 7, message.role);
            internal::sqlite::bind_text(insert_message.get(), 8, message.content);
            internal::sqlite::bind_text(insert_message.get(), 9, message.created_at);
            internal::sqlite::bind_text(insert_message.get(), 10, message.digest);
            if (internal::sqlite::step(insert_message.get()) != SQLITE_DONE)
                throw std::runtime_error(sqlite3_errmsg(db));

            TurnCheckpoint checkpoint;
            checkpoint.identity = identity;
            checkpoint.turn_id = input.consumed_turn_id;
            checkpoint.revision = 1;
            checkpoint.phase = TurnPhase::Running;
            checkpoint.continuation = TurnContinuationReason::QueuedUserInput;
            checkpoint.last_message_id = message.message_id;
            const auto checkpoint_digest = digest(encode(checkpoint));
            Statement insert_turn(db, "INSERT INTO conversation_turns VALUES(?,?,?,?,?,?,?,?,?,?)");
            bind_id(insert_turn.get(), identity);
            internal::sqlite::bind_text(insert_turn.get(), 3, checkpoint.turn_id);
            internal::sqlite::bind_uint64(insert_turn.get(), 4, checkpoint.revision);
            internal::sqlite::bind_uint64(insert_turn.get(), 5, checkpoint.iteration);
            internal::sqlite::bind_text(insert_turn.get(), 6, phase(checkpoint.phase));
            internal::sqlite::bind_text(insert_turn.get(), 7, name(checkpoint.continuation));
            internal::sqlite::bind_text(insert_turn.get(), 8, checkpoint.last_message_id);
            internal::sqlite::bind_text(insert_turn.get(), 9, checkpoint.compact_boundary_digest);
            internal::sqlite::bind_text(insert_turn.get(), 10, checkpoint_digest);
            if (internal::sqlite::step(insert_turn.get()) != SQLITE_DONE)
                throw std::runtime_error(sqlite3_errmsg(db));

            Statement event_tail(db, "SELECT head_sequence FROM conversation_event_streams WHERE tenant=? AND conversation=?");
            bind_id(event_tail.get(), identity);
            std::uint64_t event_sequence = internal::sqlite::step(event_tail.get()) == SQLITE_ROW
                ? internal::sqlite::column_uint64(event_tail.get(), 0) : 0;
            std::vector<RuntimeEventEnvelope> events;
            for (const auto *event_type : {"user_input_claimed", "user_input_consumed",
                                           "turn_created_from_queued_input", "turn_started"})
            {
                RuntimeEventEnvelope event;
                event.tenant_id = identity.tenant_id;
                event.conversation_id = identity.conversation_id;
                event.turn_id = checkpoint.turn_id;
                event.run_id = checkpoint.turn_id;
                event.sequence = ++event_sequence;
                event.event_id = checkpoint.turn_id + ":" + std::to_string(event.sequence);
                event.durability = EventDurability::Durable;
                event.visibility = EventVisibility::Operations;
                event.event_type = event_type;
                event.timestamp = input.created_at;
                event.payload = {{"input_id", input.input_id}, {"source_turn_id", input.target_turn_id},
                                 {"disposition", name(input.disposition)}};
                const auto event_json = encode(event);
                event.digest = digest(event_json);
                Statement insert_event(db, "INSERT INTO conversation_events VALUES(?,?,?,?,?)");
                bind_id(insert_event.get(), identity);
                internal::sqlite::bind_uint64(insert_event.get(), 3, event.sequence);
                internal::sqlite::bind_text(insert_event.get(), 4, event_json.dump());
                internal::sqlite::bind_text(insert_event.get(), 5, event.digest);
                if (internal::sqlite::step(insert_event.get()) != SQLITE_DONE)
                    throw std::runtime_error(sqlite3_errmsg(db));
                events.push_back(std::move(event));
            }
            Statement update_stream(db,"UPDATE conversation_event_streams SET head_sequence=?,revision=revision+1 WHERE tenant=? AND conversation=?");
            internal::sqlite::bind_uint64(update_stream.get(),1,event_sequence); internal::sqlite::bind_text(update_stream.get(),2,identity.tenant_id); internal::sqlite::bind_text(update_stream.get(),3,identity.conversation_id);
            if(internal::sqlite::step(update_stream.get())!=SQLITE_DONE || internal::sqlite::changes(db)!=1) throw std::runtime_error("event stream metadata missing");

            const auto consumed_json = encode(input);
            input.digest = digest(consumed_json);
            Statement update(db, "UPDATE conversation_inputs SET state='consumed',input_json=?,digest=? WHERE tenant=? AND conversation=? AND sequence=? AND state='queued'");
            internal::sqlite::bind_text(update.get(), 1, consumed_json.dump());
            internal::sqlite::bind_text(update.get(), 2, input.digest);
            internal::sqlite::bind_text(update.get(), 3, identity.tenant_id);
            internal::sqlite::bind_text(update.get(), 4, identity.conversation_id);
            internal::sqlite::bind_uint64(update.get(), 5, input_sequence);
            if (internal::sqlite::step(update.get()) != SQLITE_DONE || internal::sqlite::changes(db) != 1)
                throw std::runtime_error("queued input claim conflict");
            transaction.commit();
            return QueuedTurnClaim{std::move(input), std::move(checkpoint), std::move(events)};
        }
        catch (const std::exception &failure)
        {
            if (e) *e = failure.what();
            return std::nullopt;
        }
    }
}
