#include <agent/session/session_store.hpp>
#include "agent/internal/sqlite_utils.hpp"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <system_error>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace agent_framework {
namespace {

namespace sqlite = internal::sqlite;

constexpr int kSchemaVersion = 1;

std::string now_text() {
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

void exec_sql(sqlite3* db, const char* sql) {
    char* error = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        std::string message = error ? error : "sqlite error";
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

json message_to_json(const Message& m) {
    json out{{"role", m.role}, {"content", m.content}, {"timestamp", m.timestamp}};
    if (m.tool_call_id) out["tool_call_id"] = *m.tool_call_id;
    if (m.tool_name) out["tool_name"] = *m.tool_name;
    if (m.tool_result) out["tool_result"] = *m.tool_result;
    return out;
}

Message message_from_json(const json& j) {
    Message m;
    m.role = j.value("role", "");
    m.content = j.value("content", "");
    if (j.contains("tool_call_id")) m.tool_call_id = j.at("tool_call_id").get<std::string>();
    if (j.contains("tool_name")) m.tool_name = j.at("tool_name").get<std::string>();
    if (j.contains("tool_result")) m.tool_result = j.at("tool_result");
    m.timestamp = j.value("timestamp", std::time_t{});
    return m;
}

json snapshot_to_json(const SessionSnapshot& s) {
    json tools = json::array();
    for (const auto& t : s.tool_commits) {
        tools.push_back({{"tool_call_id", t.tool_call_id}, {"attempt", t.attempt},
                         {"status", t.status}, {"result_digest", t.result_digest}});
    }
    json children = json::array();
    for (const auto& c : s.child_tasks) {
        children.push_back({{"child_id", c.child_id}, {"backend", c.backend},
                            {"attempt", c.attempt}, {"status", c.status},
                            {"payload", c.payload}});
    }
    return {{"format_version", 1}, {"state", agent_thread_state_to_json(s.state)},
            {"tool_commits", std::move(tools)}, {"child_tasks", std::move(children)}};
}

SessionSnapshot snapshot_from_payload(std::string id, std::uint64_t revision,
                                      std::string checkpoint, const std::string& payload) {
    const json j = json::parse(payload);
    if (j.value("format_version", 0) != 1) {
        throw std::runtime_error("unsupported session payload version");
    }
    SessionSnapshot s;
    s.session_id = std::move(id);
    s.revision = revision;
    s.checkpoint_id = std::move(checkpoint);
    s.state = agent_thread_state_from_json(j.at("state"));
    for (const auto& t : j.value("tool_commits", json::array())) {
        s.tool_commits.push_back({t.value("tool_call_id", ""), t.value("attempt", 0U),
                                  t.value("status", ""), t.value("result_digest", "")});
    }
    for (const auto& c : j.value("child_tasks", json::array())) {
        s.child_tasks.push_back({c.value("child_id", ""), c.value("backend", ""),
                                 c.value("attempt", 0U), c.value("status", ""),
                                 c.value("payload", json::object())});
    }
    return s;
}

}  // namespace

json agent_thread_state_to_json(const internal::AgentThreadState& s) {
    json history = json::array();
    for (const auto& m : s.history) history.push_back(message_to_json(m));
    json out{{"format_version", 1}, {"history", std::move(history)},
             {"iteration", s.iteration}, {"last_error", s.last_error},
             {"initial_user_prompt", s.initial_user_prompt},
             {"verifier_retry_count", s.verifier_retry_count},
             {"last_memory_auto_compact_iteration", s.last_memory_auto_compact_iteration}};
    if (s.skill_prompt_cache) out["skill_prompt_cache"] = *s.skill_prompt_cache;
    if (s.active_skill_id) out["active_skill_id"] = *s.active_skill_id;
    if (s.last_memory_compaction_ts) out["last_memory_compaction_ts"] = *s.last_memory_compaction_ts;
    if (!s.last_memory_compaction_report.empty())
        out["last_memory_compaction_report"] = s.last_memory_compaction_report;
    if (s.execution_context) {
        json allowed = json::array();
        for (const auto& v : s.execution_context->allowed_mcp_services) allowed.push_back(v);
        out["execution_context"] = {{"cwd", s.execution_context->cwd},
            {"allowed_mcp_services", std::move(allowed)},
            {"input_policy_version", s.execution_context->input_policy_version}};
        if (s.execution_context->session_id) out["execution_context"]["session_id"] = *s.execution_context->session_id;
        if (s.execution_context->task_id) out["execution_context"]["task_id"] = *s.execution_context->task_id;
    }
    json injected = json::array();
    for (const auto& b : s.pending_injected_context) {
        json x{{"source_kind", b.source_kind}, {"source_ref", b.source_ref},
               {"text_utf8", b.text_utf8}, {"byte_length", b.byte_length}};
        if (b.mime_hint) x["mime_hint"] = *b.mime_hint;
        injected.push_back(std::move(x));
    }
    out["pending_injected_context"] = std::move(injected);
    json actions = json::array();
    for (const auto& a : s.pending_control_actions) {
        actions.push_back({{"command", a.command}, {"args", a.args}, {"raw_line", a.raw_line}});
    }
    out["pending_control_actions"] = std::move(actions);
    out["pending_input_violations"] = s.pending_input_violations;
    return out;
}

internal::AgentThreadState agent_thread_state_from_json(const json& j) {
    if (j.value("format_version", 0) != 1) throw std::runtime_error("unsupported agent state version");
    internal::AgentThreadState s;
    for (const auto& m : j.value("history", json::array())) s.history.push_back(message_from_json(m));
    s.iteration = j.value("iteration", 0);
    s.last_error = j.value("last_error", "");
    s.initial_user_prompt = j.value("initial_user_prompt", "");
    if (j.contains("skill_prompt_cache")) s.skill_prompt_cache = j.at("skill_prompt_cache").get<std::string>();
    if (j.contains("active_skill_id")) s.active_skill_id = j.at("active_skill_id").get<std::string>();
    s.verifier_retry_count = j.value("verifier_retry_count", 0);
    s.last_memory_auto_compact_iteration = j.value("last_memory_auto_compact_iteration", -1);
    if (j.contains("last_memory_compaction_ts")) s.last_memory_compaction_ts = j.at("last_memory_compaction_ts").get<std::time_t>();
    if (j.contains("last_memory_compaction_report") && j.at("last_memory_compaction_report").is_object())
        s.last_memory_compaction_report = j.at("last_memory_compaction_report");
    if (j.contains("execution_context")) {
        const auto& x = j.at("execution_context");
        ExecutionContext c;
        c.cwd = x.value("cwd", "");
        c.input_policy_version = x.value("input_policy_version", "wp27-v1");
        for (const auto& v : x.value("allowed_mcp_services", json::array())) c.allowed_mcp_services.insert(v.get<std::string>());
        if (x.contains("session_id")) c.session_id = x.at("session_id").get<std::string>();
        if (x.contains("task_id")) c.task_id = x.at("task_id").get<std::string>();
        s.execution_context = std::move(c);
    }
    for (const auto& x : j.value("pending_injected_context", json::array())) {
        InjectedContextBlock b;
        b.source_kind = x.value("source_kind", ""); b.source_ref = x.value("source_ref", "");
        b.text_utf8 = x.value("text_utf8", ""); b.byte_length = x.value("byte_length", b.text_utf8.size());
        if (x.contains("mime_hint")) b.mime_hint = x.at("mime_hint").get<std::string>();
        s.pending_injected_context.push_back(std::move(b));
    }
    for (const auto& x : j.value("pending_control_actions", json::array())) {
        s.pending_control_actions.push_back({x.value("command", ""), x.value("args", json::object()), x.value("raw_line", "")});
    }
    s.pending_input_violations = j.value("pending_input_violations", std::vector<std::string>{});
    return s;
}

SessionSnapshot InMemorySessionStore::load_or_create(std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto [it, inserted] = sessions_.try_emplace(std::string(id));
    if (inserted) it->second.session_id = std::string(id);
    return it->second;
}

std::optional<SessionSnapshot> InMemorySessionStore::load_current(std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = sessions_.find(std::string(id));
    if (found == sessions_.end()) return std::nullopt;
    return found->second;
}

SessionCommitResult InMemorySessionStore::commit(const SessionSnapshot& next, std::uint64_t expected) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto [it, inserted] = sessions_.try_emplace(next.session_id);
    if (inserted) it->second.session_id = next.session_id;
    if (it->second.revision != expected) return {SessionCommitStatus::RevisionConflict, it->second.revision, "revision conflict"};
    it->second = next; it->second.revision = expected + 1;
    return {SessionCommitStatus::Committed, it->second.revision, {}};
}

std::optional<SessionSnapshot> InMemorySessionStore::load_checkpoint(std::string_view id, std::string_view checkpoint) {
    auto s = load_or_create(id);
    if (s.checkpoint_id == checkpoint) return s;
    return std::nullopt;
}

SQLiteSessionStore::SQLiteSessionStore(std::string path, SQLiteSessionStoreOptions options)
    : path_(std::move(path)), options_(std::move(options)) {
    if (!options_.codec) options_.codec = std::make_shared<IdentitySessionPayloadCodec>();
    const std::filesystem::path p(path_);
    const bool parent_existed = !p.has_parent_path() || std::filesystem::exists(p.parent_path());
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
#if !defined(_WIN32)
    struct stat info {};
    if (!parent_existed && p.has_parent_path()) ::chmod(p.parent_path().c_str(), 0700);
    if (options_.require_private_permissions && parent_existed && p.has_parent_path() &&
        ::stat(p.parent_path().c_str(), &info) == 0 && (info.st_mode & 0077) != 0) {
        throw std::runtime_error("session database directory permissions must be 0700");
    }
    if (options_.require_private_permissions && ::stat(path_.c_str(), &info) == 0 &&
        (info.st_mode & 0077) != 0) {
        throw std::runtime_error("session database permissions must be 0600");
    }
    if (p.has_parent_path()) ::chmod(p.parent_path().c_str(), 0700);
#endif
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path_.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        std::string e = db ? sqlite3_errmsg(db) : "sqlite open failed";
        if (db) sqlite3_close(db);
        throw std::runtime_error(e);
    }
    db_ = db;
#if !defined(_WIN32)
    ::chmod(path_.c_str(), 0600);
#endif
    sqlite3_busy_timeout(db, options_.busy_timeout_ms);
    migrate();
}

SQLiteSessionStore::~SQLiteSessionStore() { if (db_) sqlite3_close(static_cast<sqlite3*>(db_)); }

void SQLiteSessionStore::migrate() {
    auto* db = static_cast<sqlite3*>(db_);
    exec_sql(db, "BEGIN IMMEDIATE");
    try {
        exec_sql(db, "CREATE TABLE IF NOT EXISTS schema_version(version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL)");
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, "SELECT COALESCE(MAX(version),0) FROM schema_version", -1, &st, nullptr);
        int version = sqlite::step(st) == SQLITE_ROW ? sqlite::column_int(st, 0) : 0;
        sqlite3_finalize(st);
        if (version > kSchemaVersion) throw std::runtime_error("session database schema is newer than this binary");
        if (version < 1) {
            exec_sql(db, "CREATE TABLE sessions(session_id TEXT PRIMARY KEY, revision INTEGER NOT NULL, checkpoint_id TEXT NOT NULL, state_payload BLOB NOT NULL, terminal_status TEXT NOT NULL, created_at TEXT NOT NULL, updated_at TEXT NOT NULL)");
            exec_sql(db, "CREATE TABLE tool_commits(session_id TEXT NOT NULL,tool_call_id TEXT NOT NULL,attempt INTEGER NOT NULL,status TEXT NOT NULL,result_digest TEXT,committed_at TEXT NOT NULL,PRIMARY KEY(session_id,tool_call_id,attempt),FOREIGN KEY(session_id) REFERENCES sessions(session_id) ON DELETE CASCADE)");
            exec_sql(db, "CREATE TABLE child_tasks(session_id TEXT NOT NULL,child_id TEXT NOT NULL,backend TEXT NOT NULL,attempt INTEGER NOT NULL,status TEXT NOT NULL,payload BLOB NOT NULL,updated_at TEXT NOT NULL,PRIMARY KEY(session_id,child_id,attempt),FOREIGN KEY(session_id) REFERENCES sessions(session_id) ON DELETE CASCADE)");
            exec_sql(db, "CREATE INDEX child_tasks_status_idx ON child_tasks(session_id,status)");
            exec_sql(db, "INSERT INTO schema_version(version,applied_at) VALUES(1,datetime('now'))");
        }
        exec_sql(db, "COMMIT");
    } catch (...) { sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); throw; }
}

SessionSnapshot SQLiteSessionStore::load_or_create(std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* db = static_cast<sqlite3*>(db_);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT revision,checkpoint_id,state_payload FROM sessions WHERE session_id=?", -1, &st, nullptr);
    sqlite::bind_text(st, 1, id);
    if (sqlite::step(st) == SQLITE_ROW) {
        auto rev = static_cast<std::uint64_t>(sqlite::column_int64(st, 0));
        std::string cp = sqlite::column_text(st, 1);
        std::string payload = sqlite::column_blob(st, 2);
        sqlite3_finalize(st);
        return snapshot_from_payload(std::string(id), rev, std::move(cp), options_.codec->decode(payload));
    }
    sqlite3_finalize(st);
    SessionSnapshot fresh; fresh.session_id = std::string(id); return fresh;
}

std::optional<SessionSnapshot> SQLiteSessionStore::load_current(std::string_view id) {
    auto snapshot = load_or_create(id);
    if (snapshot.revision == 0) return std::nullopt;
    return snapshot;
}

SessionCommitResult SQLiteSessionStore::commit(const SessionSnapshot& next, std::uint64_t expected) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* db = static_cast<sqlite3*>(db_);
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_BUSY) return {SessionCommitStatus::StoreBusy, expected, "database busy"};
    try {
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, "SELECT revision FROM sessions WHERE session_id=?", -1, &st, nullptr);
        sqlite::bind_text(st, 1, next.session_id);
        const int step = sqlite::step(st);
        const std::uint64_t current = step == SQLITE_ROW ? static_cast<std::uint64_t>(sqlite::column_int64(st, 0)) : 0;
        sqlite3_finalize(st);
        if (current != expected) {
            exec_sql(db, "ROLLBACK");
            return {SessionCommitStatus::RevisionConflict, current, "revision conflict"};
        }
        const std::string payload = options_.codec->encode(snapshot_to_json(next).dump());
        const std::string now = now_text();
        sqlite3_prepare_v2(db, "INSERT INTO sessions(session_id,revision,checkpoint_id,state_payload,terminal_status,created_at,updated_at) VALUES(?,?,?,?,?,?,?) ON CONFLICT(session_id) DO UPDATE SET revision=excluded.revision,checkpoint_id=excluded.checkpoint_id,state_payload=excluded.state_payload,terminal_status=excluded.terminal_status,updated_at=excluded.updated_at", -1, &st, nullptr);
        sqlite::bind_text(st, 1, next.session_id);
        sqlite::bind_int64(st, 2, static_cast<sqlite3_int64>(expected + 1));
        sqlite::bind_text(st, 3, next.checkpoint_id);
        sqlite::bind_blob(st, 4, payload.data(), payload.size());
        sqlite::bind_text(st, 5, "committed");
        sqlite::bind_text(st, 6, now);
        sqlite::bind_text(st, 7, now);
        if (sqlite::step(st) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
        sqlite3_finalize(st);

        sqlite3_prepare_v2(db, "DELETE FROM tool_commits WHERE session_id=?", -1, &st, nullptr);
        sqlite::bind_text(st, 1, next.session_id);
        if (sqlite::step(st) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
        sqlite3_finalize(st);
        for (const auto& tool : next.tool_commits) {
            sqlite3_prepare_v2(db, "INSERT INTO tool_commits(session_id,tool_call_id,attempt,status,result_digest,committed_at) VALUES(?,?,?,?,?,?)", -1, &st, nullptr);
            sqlite::bind_text(st, 1, next.session_id);
            sqlite::bind_text(st, 2, tool.tool_call_id);
            sqlite::bind_int64(st, 3, static_cast<sqlite3_int64>(tool.attempt));
            sqlite::bind_text(st, 4, tool.status);
            sqlite::bind_text(st, 5, tool.result_digest);
            sqlite::bind_text(st, 6, now);
            if (sqlite::step(st) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
            sqlite3_finalize(st);
        }

        sqlite3_prepare_v2(db, "DELETE FROM child_tasks WHERE session_id=?", -1, &st, nullptr);
        sqlite::bind_text(st, 1, next.session_id);
        if (sqlite::step(st) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
        sqlite3_finalize(st);
        for (const auto& child : next.child_tasks) {
            const std::string child_payload = options_.codec->encode(child.payload.dump());
            sqlite3_prepare_v2(db, "INSERT INTO child_tasks(session_id,child_id,backend,attempt,status,payload,updated_at) VALUES(?,?,?,?,?,?,?)", -1, &st, nullptr);
            sqlite::bind_text(st, 1, next.session_id);
            sqlite::bind_text(st, 2, child.child_id);
            sqlite::bind_text(st, 3, child.backend);
            sqlite::bind_int64(st, 4, static_cast<sqlite3_int64>(child.attempt));
            sqlite::bind_text(st, 5, child.status);
            sqlite::bind_blob(st, 6, child_payload.data(), child_payload.size());
            sqlite::bind_text(st, 7, now);
            if (sqlite::step(st) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
            sqlite3_finalize(st);
        }
        exec_sql(db, "COMMIT");
        return {SessionCommitStatus::Committed, expected + 1, {}};
    } catch (const std::exception& e) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        return {SessionCommitStatus::Error, expected, e.what()};
    }
}

std::optional<SessionSnapshot> SQLiteSessionStore::load_checkpoint(std::string_view id, std::string_view checkpoint) {
    auto s = load_or_create(id);
    if (s.revision != 0 && s.checkpoint_id == checkpoint) return s;
    return std::nullopt;
}

}  // namespace agent_framework
