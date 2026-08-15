#include "agent/ui/interaction_projection_store.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <sqlite3.h>

#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::ui
{
    namespace sql = agent_framework::internal::sqlite;
    namespace
    {
        int visibility(InteractionVisibility value) { return static_cast<int>(value); }
        bool valid_scope(const InteractionRef &r, std::string_view tenant, std::string_view conversation) { return r.tenant_id == tenant && (r.conversation_id.empty() || r.conversation_id == conversation); }
    }

    SQLiteInteractionProjectionStore::SQLiteInteractionProjectionStore(std::string path) : path_(std::move(path))
    {
        if (path_.empty())
            throw std::invalid_argument("interaction projection path required");
        std::error_code ec;
        auto parent = std::filesystem::path(path_).parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, ec);
        sqlite3 *opened = nullptr;
        if (ec || sqlite3_open_v2(path_.c_str(), &opened, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
        {
            std::string error = opened ? sqlite3_errmsg(opened) : ec.message();
            if (opened)
                sqlite3_close(opened);
            throw std::runtime_error(error);
        }
        db_ = opened;
        sqlite3_busy_timeout(opened, 5000);
        migrate();
    }
    SQLiteInteractionProjectionStore::~SQLiteInteractionProjectionStore()
    {
        if (db_)
            sqlite3_close(sql::database(db_));
    }
    void SQLiteInteractionProjectionStore::migrate()
    {
        auto *db = sql::database(db_);
        sql::exec(db, "PRAGMA journal_mode=WAL");
        sql::exec(db, "PRAGMA synchronous=FULL");
        sql::exec(db, "CREATE TABLE IF NOT EXISTS ui_interaction_streams(tenant TEXT NOT NULL,conversation TEXT NOT NULL,revision INTEGER NOT NULL,head_sequence INTEGER NOT NULL,updated_at TEXT NOT NULL,digest TEXT NOT NULL,PRIMARY KEY(tenant,conversation))");
        sql::exec(db, "CREATE TABLE IF NOT EXISTS ui_interaction_events(tenant TEXT NOT NULL,conversation TEXT NOT NULL,sequence INTEGER NOT NULL,event_id TEXT NOT NULL,event_json TEXT NOT NULL,digest TEXT NOT NULL,visibility INTEGER NOT NULL,PRIMARY KEY(tenant,conversation,sequence),UNIQUE(tenant,conversation,event_id))");
        sql::exec(db, "CREATE TABLE IF NOT EXISTS ui_interaction_nodes(tenant TEXT NOT NULL,conversation TEXT NOT NULL,node_id TEXT NOT NULL,revision INTEGER NOT NULL,node_json TEXT NOT NULL,digest TEXT NOT NULL,visibility INTEGER NOT NULL,PRIMARY KEY(tenant,conversation,node_id))");
        sql::exec(db, "CREATE TABLE IF NOT EXISTS ui_interaction_edges(tenant TEXT NOT NULL,conversation TEXT NOT NULL,edge_id TEXT NOT NULL,revision INTEGER NOT NULL,edge_json TEXT NOT NULL,digest TEXT NOT NULL,visibility INTEGER NOT NULL,orphan INTEGER NOT NULL,PRIMARY KEY(tenant,conversation,edge_id))");
        sql::exec(db, "CREATE INDEX IF NOT EXISTS ui_interaction_event_id_idx ON ui_interaction_events(tenant,conversation,event_id)");
        sql::exec(db, "CREATE INDEX IF NOT EXISTS ui_interaction_orphan_idx ON ui_interaction_edges(tenant,conversation,orphan)");
    }

    InteractionCommitResult SQLiteInteractionProjectionStore::commit(const InteractionCommit &commit, std::uint64_t expected)
    {
        auto event_issues = validate(commit.event);
        if (!event_issues.empty())
            return {InteractionCommitStatus::Invalid, 0, 0, "", event_issues.front().code + ":" + event_issues.front().message};
        for (const auto &node : commit.nodes)
        {
            auto issues = validate(node);
            if (!issues.empty() || !valid_scope(node.ref, commit.event.tenant_id, commit.event.conversation_id))
                return {InteractionCommitStatus::Invalid, 0, 0, "", "invalid or cross-scope node: " + node.node_id};
        }
        for (const auto &edge : commit.edges)
        {
            auto issues = validate(edge);
            if (!issues.empty())
                return {InteractionCommitStatus::Invalid, 0, 0, "", "invalid edge: " + edge.edge_id};
        }
        const auto event_doc = encode(commit.event);
        const auto event_digest = event_doc.at("digest").get<std::string>();
        std::lock_guard lock(mutex_);
        auto *db = sql::database(db_);
        try
        {
            sql::Transaction tx(db);
            std::uint64_t revision = 0, head = 0;
            std::string stream_digest;
            {
                sql::Statement q(db, "SELECT revision,head_sequence,digest FROM ui_interaction_streams WHERE tenant=? AND conversation=?");
                sql::bind_text(q.get(), 1, commit.event.tenant_id);
                sql::bind_text(q.get(), 2, commit.event.conversation_id);
                if (sql::step(q.get()) == SQLITE_ROW)
                {
                    revision = sql::column_uint64(q.get(), 0);
                    head = sql::column_uint64(q.get(), 1);
                    stream_digest = sql::column_text(q.get(), 2);
                }
            }
            if (revision != expected)
                return {InteractionCommitStatus::RevisionConflict, revision, head, stream_digest, "stream revision conflict"};
            {
                sql::Statement q(db, "SELECT digest,sequence FROM ui_interaction_events WHERE tenant=? AND conversation=? AND event_id=?");
                sql::bind_text(q.get(), 1, commit.event.tenant_id);
                sql::bind_text(q.get(), 2, commit.event.conversation_id);
                sql::bind_text(q.get(), 3, commit.event.event_id);
                if (sql::step(q.get()) == SQLITE_ROW)
                {
                    const auto existing = sql::column_text(q.get(), 0);
                    if (existing == event_digest)
                        return {InteractionCommitStatus::AlreadyExists, revision, head, existing, ""};
                    return {InteractionCommitStatus::RevisionConflict, revision, head, existing, "event id already has different digest"};
                }
            }
            if (commit.event.sequence != head + 1)
                return {InteractionCommitStatus::RevisionConflict, revision, head, stream_digest, "event sequence must append at head+1"};
            const auto upsert_node = [&](const InteractionNode &node)
            {const auto doc=encode(node);const auto d=doc.at("digest").get<std::string>();sql::Statement old(db,"SELECT revision,digest FROM ui_interaction_nodes WHERE tenant=? AND conversation=? AND node_id=?");sql::bind_text(old.get(),1,commit.event.tenant_id);sql::bind_text(old.get(),2,commit.event.conversation_id);sql::bind_text(old.get(),3,node.node_id);if(sql::step(old.get())==SQLITE_ROW){const auto old_revision=sql::column_uint64(old.get(),0);const auto old_digest=sql::column_text(old.get(),1);if(old_revision>node.revision||(old_revision==node.revision&&old_digest!=d))throw std::runtime_error("node revision conflict: "+node.node_id);if(old_revision==node.revision)return;}sql::Statement q(db,"INSERT INTO ui_interaction_nodes VALUES(?,?,?,?,?,?,?) ON CONFLICT(tenant,conversation,node_id) DO UPDATE SET revision=excluded.revision,node_json=excluded.node_json,digest=excluded.digest,visibility=excluded.visibility");sql::bind_text(q.get(),1,commit.event.tenant_id);sql::bind_text(q.get(),2,commit.event.conversation_id);sql::bind_text(q.get(),3,node.node_id);sql::bind_uint64(q.get(),4,node.revision);sql::bind_text(q.get(),5,doc.dump());sql::bind_text(q.get(),6,d);sql::bind_int(q.get(),7,visibility(node.visibility));if(sql::step(q.get())!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(db)); };
            for (const auto &node : commit.nodes)
                upsert_node(node);
            const auto node_exists = [&](std::string_view id)
            {sql::Statement q(db,"SELECT 1 FROM ui_interaction_nodes WHERE tenant=? AND conversation=? AND node_id=?");sql::bind_text(q.get(),1,commit.event.tenant_id);sql::bind_text(q.get(),2,commit.event.conversation_id);sql::bind_text(q.get(),3,id);return sql::step(q.get())==SQLITE_ROW; };
            for (const auto &edge : commit.edges)
            {
                const auto doc = encode(edge);
                const auto d = doc.at("digest").get<std::string>();
                sql::Statement old(db, "SELECT revision,digest FROM ui_interaction_edges WHERE tenant=? AND conversation=? AND edge_id=?");
                sql::bind_text(old.get(), 1, commit.event.tenant_id);
                sql::bind_text(old.get(), 2, commit.event.conversation_id);
                sql::bind_text(old.get(), 3, edge.edge_id);
                if (sql::step(old.get()) == SQLITE_ROW)
                {
                    const auto old_revision = sql::column_uint64(old.get(), 0);
                    const auto old_digest = sql::column_text(old.get(), 1);
                    if (old_revision > edge.revision || (old_revision == edge.revision && old_digest != d))
                        throw std::runtime_error("edge revision conflict: " + edge.edge_id);
                    if (old_revision == edge.revision)
                        continue;
                }
                sql::Statement q(db, "INSERT INTO ui_interaction_edges VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(tenant,conversation,edge_id) DO UPDATE SET revision=excluded.revision,edge_json=excluded.edge_json,digest=excluded.digest,visibility=excluded.visibility,orphan=excluded.orphan");
                sql::bind_text(q.get(), 1, commit.event.tenant_id);
                sql::bind_text(q.get(), 2, commit.event.conversation_id);
                sql::bind_text(q.get(), 3, edge.edge_id);
                sql::bind_uint64(q.get(), 4, edge.revision);
                sql::bind_text(q.get(), 5, doc.dump());
                sql::bind_text(q.get(), 6, d);
                sql::bind_int(q.get(), 7, visibility(edge.visibility));
                sql::bind_int(q.get(), 8, node_exists(edge.from_node_id) && node_exists(edge.to_node_id) ? 0 : 1);
                if (sql::step(q.get()) != SQLITE_DONE)
                    throw std::runtime_error(sqlite3_errmsg(db));
            }
            {
                sql::Statement q(db, "UPDATE ui_interaction_edges SET orphan=CASE WHEN EXISTS(SELECT 1 FROM ui_interaction_nodes n WHERE n.tenant=ui_interaction_edges.tenant AND n.conversation=ui_interaction_edges.conversation AND n.node_id=json_extract(ui_interaction_edges.edge_json,'$.from_node_id')) AND EXISTS(SELECT 1 FROM ui_interaction_nodes n WHERE n.tenant=ui_interaction_edges.tenant AND n.conversation=ui_interaction_edges.conversation AND n.node_id=json_extract(ui_interaction_edges.edge_json,'$.to_node_id')) THEN 0 ELSE 1 END WHERE tenant=? AND conversation=?");
                sql::bind_text(q.get(), 1, commit.event.tenant_id);
                sql::bind_text(q.get(), 2, commit.event.conversation_id);
                if (sql::step(q.get()) != SQLITE_DONE)
                    throw std::runtime_error(sqlite3_errmsg(db));
            }
            {
                sql::Statement q(db, "INSERT INTO ui_interaction_events VALUES(?,?,?,?,?,?,?)");
                sql::bind_text(q.get(), 1, commit.event.tenant_id);
                sql::bind_text(q.get(), 2, commit.event.conversation_id);
                sql::bind_uint64(q.get(), 3, commit.event.sequence);
                sql::bind_text(q.get(), 4, commit.event.event_id);
                sql::bind_text(q.get(), 5, event_doc.dump());
                sql::bind_text(q.get(), 6, event_digest);
                sql::bind_int(q.get(), 7, visibility(commit.event.visibility));
                if (sql::step(q.get()) != SQLITE_DONE)
                    throw std::runtime_error(sqlite3_errmsg(db));
            }
            const auto next_revision = revision + 1;
            const auto stream_document = nlohmann::json{{"tenant", commit.event.tenant_id}, {"conversation", commit.event.conversation_id}, {"revision", next_revision}, {"head_sequence", commit.event.sequence}, {"previous_digest", stream_digest}, {"event_digest", event_digest}};
            const auto next_digest = contracts::canonical_digest(stream_document).value_or("");
            {
                sql::Statement q(db, "INSERT INTO ui_interaction_streams VALUES(?,?,?,?,?,?) ON CONFLICT(tenant,conversation) DO UPDATE SET revision=excluded.revision,head_sequence=excluded.head_sequence,updated_at=excluded.updated_at,digest=excluded.digest");
                sql::bind_text(q.get(), 1, commit.event.tenant_id);
                sql::bind_text(q.get(), 2, commit.event.conversation_id);
                sql::bind_uint64(q.get(), 3, next_revision);
                sql::bind_uint64(q.get(), 4, commit.event.sequence);
                sql::bind_text(q.get(), 5, commit.event.timestamp);
                sql::bind_text(q.get(), 6, next_digest);
                if (sql::step(q.get()) != SQLITE_DONE)
                    throw std::runtime_error(sqlite3_errmsg(db));
            }
            tx.commit();
            return {InteractionCommitStatus::Committed, next_revision, commit.event.sequence, next_digest, ""};
        }
        catch (const std::exception &e)
        {
            return {InteractionCommitStatus::Error, 0, 0, "", e.what()};
        }
    }

    std::optional<InteractionSnapshot> SQLiteInteractionProjectionStore::snapshot(std::string_view tenant, std::string_view conversation, InteractionVisibility viewer)
    {
        std::lock_guard lock(mutex_);
        auto *db = sql::database(db_);
        InteractionSnapshot out;
        out.tenant_id = tenant;
        out.conversation_id = conversation;
        {
            sql::Statement q(db, "SELECT revision,head_sequence,updated_at,digest FROM ui_interaction_streams WHERE tenant=? AND conversation=?");
            sql::bind_text(q.get(), 1, tenant);
            sql::bind_text(q.get(), 2, conversation);
            if (sql::step(q.get()) != SQLITE_ROW)
                return std::nullopt;
            out.revision = sql::column_uint64(q.get(), 0);
            out.head_sequence = sql::column_uint64(q.get(), 1);
            out.updated_at = sql::column_text(q.get(), 2);
        }
        {
            sql::Statement q(db, "SELECT node_json FROM ui_interaction_nodes WHERE tenant=? AND conversation=? AND visibility<=? ORDER BY node_id");
            sql::bind_text(q.get(), 1, tenant);
            sql::bind_text(q.get(), 2, conversation);
            sql::bind_int(q.get(), 3, visibility(viewer));
            while (sql::step(q.get()) == SQLITE_ROW)
            {
                std::vector<contracts::ContractIssue> issues;
                auto n = decode_interaction_node(nlohmann::json::parse(sql::column_text(q.get(), 0)), &issues);
                if (!n)
                    return std::nullopt;
                out.nodes.push_back(*n);
                out.source_revisions.push_back(n->source);
            }
        }
        {
            sql::Statement q(db, "SELECT edge_json,orphan FROM ui_interaction_edges WHERE tenant=? AND conversation=? AND visibility<=? ORDER BY edge_id");
            sql::bind_text(q.get(), 1, tenant);
            sql::bind_text(q.get(), 2, conversation);
            sql::bind_int(q.get(), 3, visibility(viewer));
            while (sql::step(q.get()) == SQLITE_ROW)
            {
                std::vector<contracts::ContractIssue> issues;
                auto e = decode_interaction_edge(nlohmann::json::parse(sql::column_text(q.get(), 0)), &issues);
                if (!e)
                    return std::nullopt;
                out.edges.push_back(*e);
                out.source_revisions.push_back(e->source);
                if (sql::column_int(q.get(), 1))
                    out.orphan_edge_ids.push_back(e->edge_id);
            }
        }
        std::map<std::pair<std::string, std::string>, InteractionSourceRevision> unique;
        for (const auto &s : out.source_revisions)
            unique[{s.store, s.object_id}] = s;
        out.source_revisions.clear();
        for (auto &[_, s] : unique)
            out.source_revisions.push_back(std::move(s));
        out.digest = encode(out).at("digest");
        return out;
    }

    std::vector<UiInteractionEvent> SQLiteInteractionProjectionStore::events(std::string_view tenant, std::string_view conversation, std::uint64_t after, std::size_t limit, InteractionVisibility viewer)
    {
        std::vector<UiInteractionEvent> out;
        if (!limit)
            return out;
        limit = std::min<std::size_t>(limit, 4096);
        std::lock_guard lock(mutex_);
        auto *db = sql::database(db_);
        sql::Statement q(db, "SELECT event_json FROM ui_interaction_events WHERE tenant=? AND conversation=? AND sequence>? AND visibility<=? ORDER BY sequence LIMIT ?");
        sql::bind_text(q.get(), 1, tenant);
        sql::bind_text(q.get(), 2, conversation);
        sql::bind_uint64(q.get(), 3, after);
        sql::bind_int(q.get(), 4, visibility(viewer));
        sql::bind_uint64(q.get(), 5, limit);
        while (sql::step(q.get()) == SQLITE_ROW)
        {
            std::vector<contracts::ContractIssue> issues;
            auto e = decode_interaction_event(nlohmann::json::parse(sql::column_text(q.get(), 0)), &issues);
            if (!e)
                break;
            out.push_back(*e);
        }
        return out;
    }
    std::optional<InteractionNode> SQLiteInteractionProjectionStore::node(std::string_view tenant, std::string_view conversation, std::string_view id, InteractionVisibility viewer)
    {
        std::lock_guard lock(mutex_);
        auto *db = sql::database(db_);
        sql::Statement q(db, "SELECT node_json FROM ui_interaction_nodes WHERE tenant=? AND conversation=? AND node_id=? AND visibility<=?");
        sql::bind_text(q.get(), 1, tenant);
        sql::bind_text(q.get(), 2, conversation);
        sql::bind_text(q.get(), 3, id);
        sql::bind_int(q.get(), 4, visibility(viewer));
        if (sql::step(q.get()) != SQLITE_ROW)
            return std::nullopt;
        return decode_interaction_node(nlohmann::json::parse(sql::column_text(q.get(), 0)));
    }
} // namespace agent_framework::ui
