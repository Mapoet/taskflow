#include "agent/conversation/store.hpp"
#include "agent/internal/sqlite_utils.hpp"
#include "agent/contracts/contract.hpp"
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
        auto j = encode(v);
        v.digest = digest(j);
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
            return false;
        }
        return true;
    }
    std::vector<RuntimeEventEnvelope> SQLiteConversationStore::events(const ConversationIdentity &i, std::uint64_t after)
    {
        std::lock_guard l(mutex_);
        auto *db = internal::sqlite::database(db_);
        Statement q(db, "SELECT event_json,digest FROM conversation_events WHERE tenant=? AND conversation=? AND sequence>? ORDER BY sequence");
        bind_id(q.get(), i);
        internal::sqlite::bind_uint64(q.get(), 3, after);
        std::vector<RuntimeEventEnvelope> o;
        while (internal::sqlite::step(q.get()) == SQLITE_ROW)
        {
            try
            {
                auto j = nlohmann::json::parse(internal::sqlite::column_text(q.get(), 0));
                if (digest(j) != internal::sqlite::column_text(q.get(), 1))
                    return {};
                RuntimeEventEnvelope v;
                v.event_id = j.at("event_id");
                v.tenant_id = j.at("tenant_id");
                v.conversation_id = j.at("conversation_id");
                v.turn_id = j.at("turn_id");
                v.run_id = j.at("run_id");
                v.sequence = j.at("sequence");
                v.durability = j.at("durability") == "durable" ? EventDurability::Durable : EventDurability::Ephemeral;
                v.visibility = static_cast<EventVisibility>(j.at("visibility").get<int>());
                v.event_type = j.at("event_type");
                v.timestamp = j.at("timestamp");
                v.redaction_class = j.at("redaction_class");
                v.payload = j.at("payload");
                v.digest = internal::sqlite::column_text(q.get(), 1);
                o.push_back(std::move(v));
            }
            catch (...)
            {
                return {};
            }
        }
        return o;
    }
    std::uint64_t SQLiteConversationStore::last_event_sequence(
        const ConversationIdentity &i)
    {
        std::lock_guard l(mutex_);
        auto *db = internal::sqlite::database(db_);
        Statement q(db, "SELECT COALESCE(MAX(sequence),0) FROM conversation_events "
                        "WHERE tenant=? AND conversation=?");
        bind_id(q.get(), i);
        return internal::sqlite::step(q.get()) == SQLITE_ROW
            ? internal::sqlite::column_uint64(q.get(), 0) : 0;
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

            Statement event_tail(db, "SELECT COALESCE(MAX(sequence),0) FROM conversation_events WHERE tenant=? AND conversation=?");
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
                input.content = json.at("content");
                input.created_at = json.at("created_at");
                input.sequence = json.at("sequence");
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
}
