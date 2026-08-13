#include "agent/tool_runtime/incremental_result_store.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <sqlite3.h>

#include "agent/internal/sqlite_utils.hpp"
#include "agent/observability/audit.hpp"
#include "agent/contracts/contract.hpp"

namespace agent_framework::tool_runtime
{
    namespace s = agent_framework::internal::sqlite;
    using nlohmann::json;
    namespace
    {
        std::int64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
        bool valid_id(std::string_view v)
        {
            return !v.empty() && v.size() <= 128 && std::all_of(v.begin(), v.end(), [](unsigned char c)
                                                                { return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == ':'; });
        }
        IncrementalStreamKind parse_kind(std::string_view v)
        {
            for (int i = 0; i <= 5; ++i)
                if (name(static_cast<IncrementalStreamKind>(i)) == v)
                    return static_cast<IncrementalStreamKind>(i);
            return IncrementalStreamKind::PartialResult;
        }
        IncrementalStreamState parse_state(std::string_view v) { return v == "sealed" ? IncrementalStreamState::Sealed : v == "aborted" ? IncrementalStreamState::Aborted
                                                                                                                                        : IncrementalStreamState::Open; }
    }
    std::string_view name(IncrementalStreamKind v)
    {
        static constexpr std::string_view n[] = {"stdout", "stderr", "log", "partial_result", "checkpoint", "artifact"};
        return n[static_cast<int>(v)];
    }
    std::string_view name(IncrementalStreamState v)
    {
        static constexpr std::string_view n[] = {"open", "sealed", "aborted"};
        return n[static_cast<int>(v)];
    }
    json encode(const IncrementalManifest &v)
    {
        json chunks = json::array();
        for (const auto &c : v.chunks)
            chunks.push_back({{"sequence", c.sequence}, {"offset", c.offset}, {"size", c.size}, {"digest", c.digest}, {"media_type", c.media_type}});
        return {{"schema", "incremental-result-manifest/v1"}, {"tenant_id", v.tenant_id}, {"stream_id", v.stream_id}, {"run_id", v.run_id}, {"invocation_id", v.invocation_id}, {"attempt_id", v.attempt_id}, {"kind", name(v.kind)}, {"state", name(v.state)}, {"revision", v.revision}, {"total_size", v.total_size}, {"parent_digest", v.parent_digest}, {"media_type", v.media_type}, {"retention_class", v.retention_class}, {"redaction", {{"policy_revision", v.redaction.policy_revision}, {"rule_ids", v.redaction.rule_ids}, {"matches", v.redaction.matches}}}, {"chunks", chunks}, {"truncated", v.truncated}, {"pinned", v.pinned}};
    }
    PartialResultRef partial_result_ref(const IncrementalManifest &m, bool gain) { return {m.revision, std::string(name(m.kind)), "object://" + m.tenant_id + "/" + m.stream_id, m.manifest_digest, "application/vnd.agent.incremental-manifest+json", m.total_size, gain}; }
    IncrementalResultViewAssembler::IncrementalResultViewAssembler(IncrementalResultStore &s, std::size_t streams, std::size_t bytes) : store_(s), maximum_streams_(streams), maximum_total_bytes_(bytes)
    {
        if (!streams || !bytes)
            throw std::invalid_argument("result view budgets required");
    }
    json IncrementalResultViewAssembler::assemble(std::string_view tenant, const std::vector<PartialResultRef> &refs)
    {
        json out = {{"schema", "incremental-result-view/v1"}, {"maximum_bytes", maximum_total_bytes_}, {"streams", json::array()}};
        std::size_t remaining = maximum_total_bytes_, count = 0;
        for (auto it = refs.rbegin(); it != refs.rend() && count < maximum_streams_ && remaining; ++it)
        {
            const auto prefix = "object://" + std::string(tenant) + "/";
            if (it->uri.rfind(prefix, 0) != 0)
                continue;
            auto p = store_.preview(tenant, it->uri.substr(prefix.size()), remaining);
            if (p.manifest_digest.empty() || p.manifest_digest != it->digest)
                continue;
            out["streams"].push_back({{"kind", it->kind}, {"uri", it->uri}, {"manifest_digest", p.manifest_digest}, {"preview", p.text}, {"total_bytes", p.total_bytes}, {"included_bytes", p.included_bytes}, {"truncated", p.truncated}, {"integrity_verified", p.integrity_verified}, {"redacted", p.redacted}});
            remaining -= std::min<std::size_t>(remaining, p.included_bytes);
            ++count;
        }
        out["included_bytes"] = maximum_total_bytes_ - remaining;
        out["truncated"] = count < refs.size();
        return out;
    }
    std::optional<IncrementalManifest> decode_incremental_manifest(const json &j, std::string *error)
    {
        try
        {
            if (j.value("schema", "") != "incremental-result-manifest/v1")
                throw std::runtime_error("unsupported manifest schema");
            IncrementalManifest v;
            v.tenant_id = j.at("tenant_id");
            v.stream_id = j.at("stream_id");
            v.run_id = j.value("run_id", "");
            v.invocation_id = j.value("invocation_id", "");
            v.attempt_id = j.value("attempt_id", "");
            v.kind = parse_kind(j.at("kind").get<std::string>());
            v.state = parse_state(j.at("state").get<std::string>());
            v.revision = j.at("revision");
            v.total_size = j.at("total_size");
            v.parent_digest = j.value("parent_digest", "");
            v.media_type = j.at("media_type");
            v.retention_class = j.value("retention_class", "standard");
            v.truncated = j.value("truncated", false);
            v.pinned = j.value("pinned", false);
            auto r = j.value("redaction", json::object());
            v.redaction.policy_revision = r.value("policy_revision", "");
            v.redaction.rule_ids = r.value("rule_ids", std::vector<std::string>{});
            v.redaction.matches = r.value("matches", 0ull);
            for (auto &x : j.at("chunks"))
            {
                IncrementalChunkRef c;
                c.sequence = x.at("sequence");
                c.offset = x.at("offset");
                c.size = x.at("size");
                c.digest = x.at("digest");
                c.media_type = x.at("media_type");
                v.chunks.push_back(std::move(c));
            }
            return v;
        }
        catch (const std::exception &e)
        {
            if (error)
                *error = e.what();
            return std::nullopt;
        }
    }
    SQLiteIncrementalResultStore::SQLiteIncrementalResultStore(std::string p, distributed::ObjectStore &o, IncrementalLimits l, std::vector<RedactionRule> r, std::shared_ptr<AuditSink> a) : path_(std::move(p)), objects_(o), limits_(l), rules_(std::move(r)), audit_(std::move(a))
    {
        if (limits_.chunk_bytes == 0 || limits_.maximum_append_bytes == 0 || sqlite3_open(path_.c_str(), reinterpret_cast<sqlite3 **>(&db_)) != SQLITE_OK)
            throw std::runtime_error("incremental store open failed");
        sqlite3_busy_timeout(s::database(db_), 3000);
        migrate();
    }
    SQLiteIncrementalResultStore::~SQLiteIncrementalResultStore() { sqlite3_close(s::database(db_)); }
    void SQLiteIncrementalResultStore::migrate() { s::exec(s::database(db_), "PRAGMA journal_mode=WAL; CREATE TABLE IF NOT EXISTS incremental_streams(tenant_id TEXT NOT NULL,stream_id TEXT NOT NULL,revision INTEGER NOT NULL,manifest_digest TEXT NOT NULL,manifest_json TEXT NOT NULL,state TEXT NOT NULL,updated_at_ms INTEGER NOT NULL,PRIMARY KEY(tenant_id,stream_id)); CREATE TABLE IF NOT EXISTS incremental_appends(tenant_id TEXT NOT NULL,stream_id TEXT NOT NULL,idempotency_key TEXT NOT NULL,revision INTEGER NOT NULL,request_digest TEXT NOT NULL,PRIMARY KEY(tenant_id,stream_id,idempotency_key)); CREATE TABLE IF NOT EXISTS incremental_objects(tenant_id TEXT NOT NULL,digest TEXT NOT NULL,size INTEGER NOT NULL,kind TEXT NOT NULL,created_at_ms INTEGER NOT NULL,referenced INTEGER NOT NULL DEFAULT 1,PRIMARY KEY(tenant_id,digest,kind)); CREATE TABLE IF NOT EXISTS incremental_edges(tenant_id TEXT NOT NULL,stream_id TEXT NOT NULL,manifest_digest TEXT NOT NULL,object_digest TEXT NOT NULL,PRIMARY KEY(tenant_id,manifest_digest,object_digest)); CREATE TABLE IF NOT EXISTS incremental_tombstones(tenant_id TEXT NOT NULL,digest TEXT NOT NULL,size INTEGER NOT NULL,deleted_at_ms INTEGER NOT NULL,PRIMARY KEY(tenant_id,digest)); CREATE TABLE IF NOT EXISTS incremental_redaction_state(tenant_id TEXT NOT NULL,stream_id TEXT NOT NULL,rule_id TEXT NOT NULL,prefix_length INTEGER NOT NULL,policy_revision TEXT NOT NULL,PRIMARY KEY(tenant_id,stream_id)); CREATE INDEX IF NOT EXISTS incremental_objects_gc ON incremental_objects(referenced,created_at_ms);"); }
    void SQLiteIncrementalResultStore::audit(std::string_view event, const IncrementalManifest &m, std::string_view outcome, std::string_view error) const noexcept
    {
        if (!audit_)
            return;
        AuditEvent e;
        e.timestamp = audit_timestamp_now();
        e.tenant_id = m.tenant_id;
        e.task_id = m.run_id;
        e.component = "incremental_result_store";
        e.event_kind = std::string(event);
        e.outcome = std::string(outcome);
        e.error_code = std::string(error);
        e.payload = {{"stream_id", m.stream_id}, {"revision", m.revision}, {"manifest_digest", m.manifest_digest}, {"total_size", m.total_size}};
        e.payload_digest = audit_payload_digest(e.payload);
        audit_->write(e);
    }
    std::string SQLiteIncrementalResultStore::redact(std::string_view input, RedactionReceipt &receipt) const
    {
        std::string out(input);
        for (const auto &rule : rules_)
        {
            if (rule.literal.empty())
                continue;
            std::size_t pos = 0, count = 0;
            while ((pos = out.find(rule.literal, pos)) != std::string::npos)
            {
                out.replace(pos, rule.literal.size(), rule.replacement);
                pos += rule.replacement.size();
                ++count;
            }
            if (count)
            {
                receipt.rule_ids.push_back(rule.id);
                receipt.matches += count;
            }
        }
        return out;
    }
    std::string SQLiteIncrementalResultStore::redact_stream(std::string_view tenant, std::string_view stream, std::string_view input, RedactionReceipt &receipt)
    {
        std::string combined, prior_rule;
        std::size_t prior_length = 0;
        s::Statement q(s::database(db_), "SELECT rule_id,prefix_length,policy_revision FROM incremental_redaction_state WHERE tenant_id=? AND stream_id=?");
        s::bind_text(q.get(), 1, tenant);
        s::bind_text(q.get(), 2, stream);
        if (s::step(q.get()) == SQLITE_ROW)
        {
            prior_rule = s::column_text(q.get(), 0);
            prior_length = s::column_uint64(q.get(), 1);
            if (s::column_text(q.get(), 2) != receipt.policy_revision)
                throw std::runtime_error("redaction policy revision mismatch");
            for (const auto &rule : rules_)
                if (rule.id == prior_rule)
                    combined = rule.literal.substr(0, prior_length);
        }
        combined.append(input);
        std::string pending_rule;
        std::size_t pending = 0;
        for (const auto &rule : rules_)
            for (std::size_t n = 1; n < rule.literal.size() && n <= combined.size(); ++n)
                if (n > pending && combined.compare(combined.size() - n, n, rule.literal, 0, n) == 0)
                {
                    pending = n;
                    pending_rule = rule.id;
                }
        auto out = redact(combined.substr(0, combined.size() - pending), receipt);
        s::Statement d(s::database(db_), "DELETE FROM incremental_redaction_state WHERE tenant_id=? AND stream_id=?");
        s::bind_text(d.get(), 1, tenant);
        s::bind_text(d.get(), 2, stream);
        s::step(d.get());
        if (pending)
        {
            s::Statement i(s::database(db_), "INSERT INTO incremental_redaction_state VALUES(?,?,?,?,?)");
            s::bind_text(i.get(), 1, tenant);
            s::bind_text(i.get(), 2, stream);
            s::bind_text(i.get(), 3, pending_rule);
            s::bind_uint64(i.get(), 4, pending);
            s::bind_text(i.get(), 5, receipt.policy_revision);
            s::step(i.get());
        }
        if (prior_length && receipt.matches)
            metrics_.cross_append_matches++;
        return out;
    }
    IncrementalResult SQLiteIncrementalResultStore::open(const IncrementalOpenRequest &r)
    {
        std::scoped_lock lock(mutex_);
        IncrementalResult out;
        if (!valid_id(r.tenant_id) || !valid_id(r.stream_id) || r.media_type.empty())
        {
            out.status = IncrementalStatus::Invalid;
            out.error = "invalid stream identity or media type";
            return out;
        }
        s::Transaction tx(s::database(db_));
        s::Statement q(s::database(db_), "SELECT manifest_json,manifest_digest FROM incremental_streams WHERE tenant_id=? AND stream_id=?");
        s::bind_text(q.get(), 1, r.tenant_id);
        s::bind_text(q.get(), 2, r.stream_id);
        if (s::step(q.get()) == SQLITE_ROW)
        {
            out.manifest = *decode_incremental_manifest(json::parse(s::column_text(q.get(), 0)));
            out.manifest.manifest_digest = s::column_text(q.get(), 1);
            out.status = IncrementalStatus::AlreadyApplied;
            tx.commit();
            return out;
        }
        IncrementalManifest m;
        m.tenant_id = r.tenant_id;
        m.stream_id = r.stream_id;
        m.run_id = r.run_id;
        m.invocation_id = r.invocation_id;
        m.attempt_id = r.attempt_id;
        m.kind = r.kind;
        m.media_type = r.media_type;
        m.retention_class = r.retention_class;
        m.redaction.policy_revision = r.redaction_policy_revision;
        auto body = encode(m).dump();
        std::string error;
        auto ref = objects_.put(m.tenant_id, body, "application/vnd.agent.incremental-manifest+json", {}, &error);
        if (!ref)
        {
            out.status = IncrementalStatus::Error;
            out.error = error;
            return out;
        }
        m.manifest_digest = ref->digest;
        s::Statement i(s::database(db_), "INSERT INTO incremental_streams VALUES(?,?,?,?,?,?,?)");
        s::bind_text(i.get(), 1, m.tenant_id);
        s::bind_text(i.get(), 2, m.stream_id);
        s::bind_uint64(i.get(), 3, m.revision);
        s::bind_text(i.get(), 4, m.manifest_digest);
        s::bind_text(i.get(), 5, body);
        s::bind_text(i.get(), 6, name(m.state));
        s::bind_int64(i.get(), 7, now_ms());
        if (s::step(i.get()) != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(s::database(db_)));
        s::Statement oi(s::database(db_), "INSERT OR IGNORE INTO incremental_objects VALUES(?,?,?,?,?,1)");
        s::bind_text(oi.get(), 1, m.tenant_id);
        s::bind_text(oi.get(), 2, m.manifest_digest);
        s::bind_uint64(oi.get(), 3, ref->size);
        s::bind_text(oi.get(), 4, "manifest");
        s::bind_int64(oi.get(), 5, now_ms());
        s::step(oi.get());
        tx.commit();
        metrics_.streams_opened++;
        out.status = IncrementalStatus::Committed;
        out.manifest = m;
        audit("stream_open", m, "committed");
        return out;
    }
    std::optional<IncrementalManifest> SQLiteIncrementalResultStore::load(std::string_view tenant, std::string_view stream)
    {
        std::scoped_lock lock(mutex_);
        s::Statement q(s::database(db_), "SELECT manifest_json,manifest_digest FROM incremental_streams WHERE tenant_id=? AND stream_id=?");
        s::bind_text(q.get(), 1, tenant);
        s::bind_text(q.get(), 2, stream);
        if (s::step(q.get()) != SQLITE_ROW)
            return {};
        auto m = decode_incremental_manifest(json::parse(s::column_text(q.get(), 0)));
        if (m)
            m->manifest_digest = s::column_text(q.get(), 1);
        return m;
    }
    IncrementalResult SQLiteIncrementalResultStore::append(const IncrementalAppendRequest &r)
    {
        std::scoped_lock lock(mutex_);
        IncrementalResult out;
        if (r.idempotency_key.empty() || r.bytes.size() > limits_.maximum_append_bytes)
        {
            out.status = r.idempotency_key.empty() ? IncrementalStatus::Invalid : IncrementalStatus::LimitExceeded;
            out.error = "idempotency key required or append limit exceeded";
            return out;
        }
        s::Transaction tx(s::database(db_));
        s::Statement q(s::database(db_), "SELECT manifest_json,manifest_digest,revision,state FROM incremental_streams WHERE tenant_id=? AND stream_id=?");
        s::bind_text(q.get(), 1, r.tenant_id);
        s::bind_text(q.get(), 2, r.stream_id);
        if (s::step(q.get()) != SQLITE_ROW)
        {
            out.status = IncrementalStatus::NotFound;
            return out;
        }
        auto m = *decode_incremental_manifest(json::parse(s::column_text(q.get(), 0)));
        m.manifest_digest = s::column_text(q.get(), 1);
        auto revision = s::column_uint64(q.get(), 2);
        if (parse_state(s::column_text(q.get(), 3)) != IncrementalStreamState::Open)
        {
            out.status = IncrementalStatus::Sealed;
            out.manifest = m;
            return out;
        }
        s::Statement prior(s::database(db_), "SELECT revision,request_digest FROM incremental_appends WHERE tenant_id=? AND stream_id=? AND idempotency_key=?");
        s::bind_text(prior.get(), 1, r.tenant_id);
        s::bind_text(prior.get(), 2, r.stream_id);
        s::bind_text(prior.get(), 3, r.idempotency_key);
        if (s::step(prior.get()) == SQLITE_ROW)
        {
            out.status = IncrementalStatus::AlreadyApplied;
            out.manifest = m;
            tx.commit();
            return out;
        }
        if (revision != r.expected_revision)
        {
            metrics_.cas_conflicts++;
            out.status = IncrementalStatus::Conflict;
            out.manifest = m;
            out.error = "manifest revision conflict";
            return out;
        }
        RedactionReceipt receipt = m.redaction;
        auto sanitized = redact_stream(r.tenant_id, r.stream_id, r.bytes, receipt);
        if (m.total_size + sanitized.size() > limits_.maximum_stream_bytes)
        {
            out.status = IncrementalStatus::LimitExceeded;
            out.error = "stream size limit exceeded";
            return out;
        }
        const auto parent = m.manifest_digest;
        std::string error;
        for (std::size_t pos = 0; pos < sanitized.size() || pos == 0; pos += limits_.chunk_bytes)
        {
            auto bytes = std::string_view(sanitized).substr(pos, std::min(limits_.chunk_bytes, sanitized.size() - pos));
            auto ref = objects_.put(m.tenant_id, bytes, m.media_type, {}, &error);
            if (!ref)
            {
                out.status = IncrementalStatus::Error;
                out.error = error;
                return out;
            }
            bool duplicate = false;
            s::Statement known(s::database(db_), "SELECT 1 FROM incremental_objects WHERE tenant_id=? AND digest=? AND kind='chunk'");
            s::bind_text(known.get(), 1, m.tenant_id);
            s::bind_text(known.get(), 2, ref->digest);
            duplicate = s::step(known.get()) == SQLITE_ROW;
            s::Statement oi(s::database(db_), "INSERT OR IGNORE INTO incremental_objects VALUES(?,?,?,?,?,1)");
            s::bind_text(oi.get(), 1, m.tenant_id);
            s::bind_text(oi.get(), 2, ref->digest);
            s::bind_uint64(oi.get(), 3, ref->size);
            s::bind_text(oi.get(), 4, "chunk");
            s::bind_int64(oi.get(), 5, now_ms());
            s::step(oi.get());
            m.chunks.push_back({static_cast<std::uint64_t>(m.chunks.size() + 1), m.total_size, ref->size, ref->digest, m.media_type});
            m.total_size += ref->size;
            metrics_.chunks_written++;
            if (duplicate)
            {
                metrics_.chunks_deduplicated++;
                out.deduplicated = true;
            }
            if (sanitized.empty())
                break;
        }
        m.revision++;
        m.parent_digest = parent;
        m.redaction = std::move(receipt);
        auto body = encode(m).dump();
        auto manifest = objects_.put(m.tenant_id, body, "application/vnd.agent.incremental-manifest+json", {}, &error);
        if (!manifest)
        {
            out.status = IncrementalStatus::Error;
            out.error = error;
            return out;
        }
        m.manifest_digest = manifest->digest;
        s::Statement u(s::database(db_), "UPDATE incremental_streams SET revision=?,manifest_digest=?,manifest_json=?,updated_at_ms=? WHERE tenant_id=? AND stream_id=? AND revision=? AND state='open'");
        s::bind_uint64(u.get(), 1, m.revision);
        s::bind_text(u.get(), 2, m.manifest_digest);
        s::bind_text(u.get(), 3, body);
        s::bind_int64(u.get(), 4, now_ms());
        s::bind_text(u.get(), 5, m.tenant_id);
        s::bind_text(u.get(), 6, m.stream_id);
        s::bind_uint64(u.get(), 7, r.expected_revision);
        if (s::step(u.get()) != SQLITE_DONE || s::changes(s::database(db_)) != 1)
        {
            out.status = IncrementalStatus::Conflict;
            return out;
        }
        s::Statement ai(s::database(db_), "INSERT INTO incremental_appends VALUES(?,?,?,?,?)");
        s::bind_text(ai.get(), 1, m.tenant_id);
        s::bind_text(ai.get(), 2, m.stream_id);
        s::bind_text(ai.get(), 3, r.idempotency_key);
        s::bind_uint64(ai.get(), 4, m.revision);
        s::bind_text(ai.get(), 5, m.manifest_digest);
        s::step(ai.get());
        s::Statement mi(s::database(db_), "INSERT OR IGNORE INTO incremental_objects VALUES(?,?,?,?,?,1)");
        s::bind_text(mi.get(), 1, m.tenant_id);
        s::bind_text(mi.get(), 2, m.manifest_digest);
        s::bind_uint64(mi.get(), 3, manifest->size);
        s::bind_text(mi.get(), 4, "manifest");
        s::bind_int64(mi.get(), 5, now_ms());
        s::step(mi.get());
        tx.commit();
        metrics_.bytes_appended += sanitized.size();
        metrics_.redaction_matches += m.redaction.matches - out.manifest.redaction.matches;
        out.status = IncrementalStatus::Committed;
        out.manifest = m;
        audit("stream_append", m, "committed");
        return out;
    }
    IncrementalResult SQLiteIncrementalResultStore::transition(std::string_view tenant, std::string_view stream, std::uint64_t expected, IncrementalStreamState state)
    {
        IncrementalResult out;
        s::Transaction tx(s::database(db_));
        s::Statement q(s::database(db_), "SELECT manifest_json,manifest_digest,revision,state FROM incremental_streams WHERE tenant_id=? AND stream_id=?");
        s::bind_text(q.get(), 1, tenant);
        s::bind_text(q.get(), 2, stream);
        if (s::step(q.get()) != SQLITE_ROW)
        {
            out.status = IncrementalStatus::NotFound;
            return out;
        }
        auto m = *decode_incremental_manifest(json::parse(s::column_text(q.get(), 0)));
        m.manifest_digest = s::column_text(q.get(), 1);
        if (m.state == state)
        {
            out.status = IncrementalStatus::AlreadyApplied;
            out.manifest = m;
            tx.commit();
            return out;
        }
        if (m.state != IncrementalStreamState::Open)
        {
            out.status = IncrementalStatus::Sealed;
            out.manifest = m;
            return out;
        }
        if (m.revision != expected)
        {
            out.status = IncrementalStatus::Conflict;
            out.manifest = m;
            return out;
        }
        if(state==IncrementalStreamState::Sealed){s::Statement pending(s::database(db_),"SELECT rule_id FROM incremental_redaction_state WHERE tenant_id=? AND stream_id=?");s::bind_text(pending.get(),1,tenant);s::bind_text(pending.get(),2,stream);if(s::step(pending.get())==SQLITE_ROW){const auto rule_id=s::column_text(pending.get(),0);std::string replacement="[REDACTED]";for(const auto& rule:rules_)if(rule.id==rule_id)replacement=rule.replacement;std::string object_error;auto chunk=objects_.put(m.tenant_id,replacement,m.media_type,{},&object_error);if(!chunk){out.error=object_error;return out;}m.chunks.push_back({static_cast<std::uint64_t>(m.chunks.size()+1),m.total_size,chunk->size,chunk->digest,m.media_type});m.total_size+=chunk->size;m.redaction.matches++;m.redaction.rule_ids.push_back(rule_id);s::Statement oi(s::database(db_),"INSERT OR IGNORE INTO incremental_objects VALUES(?,?,?,?,?,1)");s::bind_text(oi.get(),1,m.tenant_id);s::bind_text(oi.get(),2,chunk->digest);s::bind_uint64(oi.get(),3,chunk->size);s::bind_text(oi.get(),4,"chunk");s::bind_int64(oi.get(),5,now_ms());s::step(oi.get());s::Statement clear(s::database(db_),"DELETE FROM incremental_redaction_state WHERE tenant_id=? AND stream_id=?");s::bind_text(clear.get(),1,tenant);s::bind_text(clear.get(),2,stream);s::step(clear.get());}}
        else {s::Statement clear(s::database(db_),"DELETE FROM incremental_redaction_state WHERE tenant_id=? AND stream_id=?");s::bind_text(clear.get(),1,tenant);s::bind_text(clear.get(),2,stream);s::step(clear.get());}
        m.parent_digest = m.manifest_digest;
        m.state = state;
        m.revision++;
        auto body = encode(m).dump();
        std::string error;
        auto ref = objects_.put(m.tenant_id, body, "application/vnd.agent.incremental-manifest+json", {}, &error);
        if (!ref)
        {
            out.error = error;
            return out;
        }
        m.manifest_digest = ref->digest;
        s::Statement u(s::database(db_), "UPDATE incremental_streams SET revision=?,manifest_digest=?,manifest_json=?,state=?,updated_at_ms=? WHERE tenant_id=? AND stream_id=? AND revision=? AND state='open'");
        s::bind_uint64(u.get(), 1, m.revision);
        s::bind_text(u.get(), 2, m.manifest_digest);
        s::bind_text(u.get(), 3, body);
        s::bind_text(u.get(), 4, name(state));
        s::bind_int64(u.get(), 5, now_ms());
        s::bind_text(u.get(), 6, tenant);
        s::bind_text(u.get(), 7, stream);
        s::bind_uint64(u.get(), 8, expected);
        s::step(u.get());
        if (s::changes(s::database(db_)) != 1)
        {
            out.status = IncrementalStatus::Conflict;
            return out;
        }
        tx.commit();
        out.status = IncrementalStatus::Committed;
        out.manifest = m;
        audit(state == IncrementalStreamState::Sealed ? "stream_seal" : "stream_abort", m, "committed");
        return out;
    }
    IncrementalResult SQLiteIncrementalResultStore::seal(std::string_view t, std::string_view i, std::uint64_t r)
    {
        std::scoped_lock lock(mutex_);
        return transition(t, i, r, IncrementalStreamState::Sealed);
    }
    IncrementalResult SQLiteIncrementalResultStore::abort(std::string_view t, std::string_view i, std::uint64_t r)
    {
        std::scoped_lock lock(mutex_);
        return transition(t, i, r, IncrementalStreamState::Aborted);
    }
    bool SQLiteIncrementalResultStore::verify(std::string_view tenant, std::string_view stream, std::string *error)
    {
        auto m = load(tenant, stream);
        if (!m)
        {
            if (error)
                *error = "stream not found";
            return false;
        }
        const auto body = encode(*m).dump();
        distributed::ObjectRef manifest{m->tenant_id, m->manifest_digest, body.size(), "application/vnd.agent.incremental-manifest+json"};
        auto persisted = objects_.get(manifest, error);
        if (!persisted || *persisted != body)
        {
            metrics_.integrity_failures++;
            if (error && error->empty())
                *error = "manifest registry/object mismatch";
            return false;
        }
        std::uint64_t offset = 0;
        for (const auto &c : m->chunks)
        {
            if (c.offset != offset)
            {
                if (error)
                    *error = "non-contiguous chunk offsets";
                metrics_.integrity_failures++;
                return false;
            }
            distributed::ObjectRef ref{m->tenant_id, c.digest, c.size, c.media_type};
            if (!objects_.get(ref, error))
            {
                metrics_.integrity_failures++;
                return false;
            }
            offset += c.size;
        }
        if (offset != m->total_size)
        {
            if (error)
                *error = "manifest total size mismatch";
            metrics_.integrity_failures++;
            return false;
        }
        return true;
    }
    IncrementalPreview SQLiteIncrementalResultStore::preview(std::string_view tenant, std::string_view stream, std::size_t maximum)
    {
        IncrementalPreview p;
        auto m = load(tenant, stream);
        if (!m)
            return p;
        maximum = maximum ? maximum : limits_.preview_bytes;
        p.manifest_digest = m->manifest_digest;
        p.total_bytes = m->total_size;
        p.redacted = !m->redaction.policy_revision.empty();
        std::string error;
        for (const auto &c : m->chunks)
        {
            if (p.text.size() >= maximum)
                break;
            distributed::ObjectRef ref{m->tenant_id, c.digest, c.size, c.media_type};
            auto bytes = objects_.get(ref, &error);
            if (!bytes)
                return p;
            auto take = std::min(maximum - p.text.size(), bytes->size());
            p.text.append(bytes->data(), take);
            p.included_bytes += take;
            p.references.push_back(c);
        }
        p.truncated = p.included_bytes < p.total_bytes;
        if (p.truncated)
            metrics_.preview_truncations++;
        p.integrity_verified = error.empty();
        return p;
    }
    IncrementalMetrics SQLiteIncrementalResultStore::metrics() const
    {
        std::scoped_lock lock(mutex_);
        return metrics_;
    }
    std::uint64_t SQLiteIncrementalResultStore::mark_orphans(std::int64_t older)
    {
        std::scoped_lock lock(mutex_);
        s::Statement heads(s::database(db_), "SELECT tenant_id,stream_id,manifest_digest,manifest_json FROM incremental_streams");
        while (s::step(heads.get()) == SQLITE_ROW)
        {
            auto m = decode_incremental_manifest(json::parse(s::column_text(heads.get(), 3)));
            if (!m)
                continue;
            for (const auto &chunk : m->chunks)
            {
                s::Statement edge(s::database(db_), "INSERT OR IGNORE INTO incremental_edges VALUES(?,?,?,?)");
                s::bind_text(edge.get(), 1, s::column_text(heads.get(), 0));
                s::bind_text(edge.get(), 2, s::column_text(heads.get(), 1));
                s::bind_text(edge.get(), 3, s::column_text(heads.get(), 2));
                s::bind_text(edge.get(), 4, chunk.digest);
                s::step(edge.get());
            }
        }
        s::Statement u(s::database(db_), "UPDATE incremental_objects SET referenced=0 WHERE created_at_ms<? AND NOT EXISTS(SELECT 1 FROM incremental_edges e WHERE e.tenant_id=incremental_objects.tenant_id AND e.object_digest=incremental_objects.digest) AND NOT EXISTS(SELECT 1 FROM incremental_streams h WHERE h.tenant_id=incremental_objects.tenant_id AND h.manifest_digest=incremental_objects.digest)");
        s::bind_int64(u.get(), 1, older);
        s::step(u.get());
        auto n = static_cast<std::uint64_t>(s::changes(s::database(db_)));
        metrics_.orphan_objects += n;
        return n;
    }
    bool SQLiteIncrementalResultStore::set_pinned(std::string_view tenant, std::string_view stream, bool pinned)
    {
        std::scoped_lock lock(mutex_);
        s::Statement q(s::database(db_), "SELECT manifest_json FROM incremental_streams WHERE tenant_id=? AND stream_id=?");
        s::bind_text(q.get(), 1, tenant);
        s::bind_text(q.get(), 2, stream);
        if (s::step(q.get()) != SQLITE_ROW)
            return false;
        auto m = decode_incremental_manifest(json::parse(s::column_text(q.get(), 0)));
        if (!m)
            return false;
        m->pinned = pinned;
        s::Statement u(s::database(db_), "UPDATE incremental_streams SET manifest_json=? WHERE tenant_id=? AND stream_id=?");
        s::bind_text(u.get(), 1, encode(*m).dump());
        s::bind_text(u.get(), 2, tenant);
        s::bind_text(u.get(), 3, stream);
        return s::step(u.get()) == SQLITE_DONE && s::changes(s::database(db_)) == 1;
    }
    std::uint64_t SQLiteIncrementalResultStore::reconcile_objects(std::string_view tenant, std::size_t maximum)
    {
        if (!objects_.capabilities().list)
            return 0;
        std::uint64_t found = 0;
        std::string cursor;
        do
        {
            auto page = objects_.list(tenant, cursor, std::min<std::size_t>(maximum - found, 100));
            if (!page.error.empty())
                break;
            std::scoped_lock lock(mutex_);
            for (const auto &ref : page.objects)
            {
                s::Statement i(s::database(db_), "INSERT OR IGNORE INTO incremental_objects VALUES(?,?,?,?,?,0)");
                s::bind_text(i.get(), 1, tenant);
                s::bind_text(i.get(), 2, ref.digest);
                s::bind_uint64(i.get(), 3, ref.size);
                s::bind_text(i.get(), 4, "reconciled");
                s::bind_int64(i.get(), 5, now_ms());
                s::step(i.get());
                found += s::changes(s::database(db_));
            }
            cursor = page.next_cursor;
            if (page.complete)
                break;
        } while (found < maximum);
        metrics_.orphan_objects += found;
        return found;
    }
    IncrementalGcResult SQLiteIncrementalResultStore::collect(const IncrementalGcPolicy &policy)
    {
        std::scoped_lock lock(mutex_);
        IncrementalGcResult out;
        if (!objects_.capabilities().remove)
        {
            out.error = "object removal unsupported";
            return out;
        }
        s::Statement q(s::database(db_), "SELECT tenant_id,digest,size FROM incremental_objects o WHERE referenced=0 AND created_at_ms<? AND NOT EXISTS(SELECT 1 FROM incremental_edges e WHERE e.tenant_id=o.tenant_id AND e.object_digest=o.digest) AND NOT EXISTS(SELECT 1 FROM incremental_streams h WHERE h.tenant_id=o.tenant_id AND h.manifest_digest=o.digest) LIMIT ?");
        s::bind_int64(q.get(), 1, policy.grace_before_ms);
        s::bind_uint64(q.get(), 2, policy.maximum_objects);
        while (s::step(q.get()) == SQLITE_ROW)
        {
            out.candidates++;
            if (policy.dry_run)
                continue;
            distributed::ObjectRef ref{s::column_text(q.get(), 0), s::column_text(q.get(), 1), s::column_uint64(q.get(), 2), "application/octet-stream"};
            auto removed = objects_.remove(ref);
            if (!removed)
            {
                out.quarantined++;
                continue;
            }
            s::Statement t(s::database(db_), "INSERT OR REPLACE INTO incremental_tombstones VALUES(?,?,?,?)");
            s::bind_text(t.get(), 1, ref.tenant_id);
            s::bind_text(t.get(), 2, ref.digest);
            s::bind_uint64(t.get(), 3, ref.size);
            s::bind_int64(t.get(), 4, now_ms());
            s::step(t.get());
            s::Statement d(s::database(db_), "DELETE FROM incremental_objects WHERE tenant_id=? AND digest=?");
            s::bind_text(d.get(), 1, ref.tenant_id);
            s::bind_text(d.get(), 2, ref.digest);
            s::step(d.get());
            out.deleted++;
            out.reclaimed_bytes += ref.size;
        }
        metrics_.objects_deleted += out.deleted;
        metrics_.bytes_reclaimed += out.reclaimed_bytes;
        return out;
    }
} // namespace agent_framework::tool_runtime
