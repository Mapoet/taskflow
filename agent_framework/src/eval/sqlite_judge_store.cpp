#include "agent/eval/judge_workflow.hpp"

#include <filesystem>
#include <stdexcept>

#include <sqlite3.h>

#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::eval
{
    namespace
    {
        namespace sqlite = internal::sqlite;

        bool valid_checkpoint(const JudgeCheckpoint &v, std::string *error)
        {
            if (v.metadata.identity.tenant_id.empty() || v.metadata.identity.task_id.empty() ||
                v.workflow_id.empty() || v.revision == 0 || v.suite_digest.empty() ||
                v.baseline_run_digest.empty() || v.candidate_run_digest.empty() ||
                v.memory_snapshot_id.empty() || v.memory_view_digest.empty())
            {
                if (error)
                    *error = "identity, workflow, revision, input digests and pinned memory view are required";
                return false;
            }
            return true;
        }
        bool immutable_match(const JudgeCheckpoint &a, const JudgeCheckpoint &b)
        {
            return a.metadata.identity.tenant_id == b.metadata.identity.tenant_id &&
                   a.metadata.identity.task_id == b.metadata.identity.task_id &&
                   a.workflow_id == b.workflow_id && a.suite_digest == b.suite_digest &&
                   a.baseline_run_digest == b.baseline_run_digest &&
                   a.candidate_run_digest == b.candidate_run_digest &&
                   a.memory_snapshot_id == b.memory_snapshot_id &&
                   a.memory_view_digest == b.memory_view_digest;
        }
        bool terminal(const JudgeCheckpoint &v)
        {
            return v.state == JudgeWorkflowState::Approved || v.state == JudgeWorkflowState::Rejected ||
                   v.state == JudgeWorkflowState::ManualReview;
        }
        bool report_match(const JudgeCheckpoint &checkpoint, const EvaluationReport &report)
        {
            if (report.workflow_id != checkpoint.workflow_id ||
                report.metadata.identity.tenant_id != checkpoint.metadata.identity.tenant_id ||
                report.metadata.identity.task_id != checkpoint.metadata.identity.task_id ||
                report.suite_digest != checkpoint.suite_digest ||
                report.baseline_run_digest != checkpoint.baseline_run_digest ||
                report.candidate_run_digest != checkpoint.candidate_run_digest || !report.executed ||
                checkpoint.evaluation_report_digest !=
                    encode(report).at("canonical_digest").get<std::string>())
                return false;
            if (checkpoint.state == JudgeWorkflowState::Approved)
                return report.decision.outcome == UpgradeOutcome::Approved;
            if (checkpoint.state == JudgeWorkflowState::Rejected)
                return report.decision.outcome == UpgradeOutcome::Rejected;
            if (checkpoint.state == JudgeWorkflowState::ManualReview)
                return report.decision.outcome == UpgradeOutcome::ManualReview;
            return false;
        }
        JudgeStoreCommit failure(sqlite3 *db, int rc)
        {
            return {rc == SQLITE_BUSY || rc == SQLITE_LOCKED ? JudgeStoreStatus::Busy : JudgeStoreStatus::Error,
                    0,
                    {},
                    sqlite3_errmsg(db)};
        }
    } // namespace

    std::string InMemoryJudgeStore::key(std::string_view tenant, std::string_view workflow)
    {
        return std::string(tenant) + '\n' + std::string(workflow);
    }
    JudgeStoreCommit InMemoryJudgeStore::create(const JudgeCheckpoint &v)
    {
        std::string error;
        if (!valid_checkpoint(v, &error) || v.revision != 1)
            return {JudgeStoreStatus::Invalid, 0, {}, error.empty() ? "initial revision must be 1" : error};
        std::lock_guard lock(mutex_);
        const auto id = key(v.metadata.identity.tenant_id, v.workflow_id);
        if (checkpoints_.count(id))
            return {JudgeStoreStatus::AlreadyExists, 0, {}, {}};
        checkpoints_[id] = {v, v.revision};
        return {JudgeStoreStatus::Committed, v.revision, encode(v).at("canonical_digest").get<std::string>(), {}};
    }
    std::optional<StoredJudgeCheckpoint> InMemoryJudgeStore::load(std::string_view tenant, std::string_view workflow)
    {
        std::lock_guard lock(mutex_);
        auto it = checkpoints_.find(key(tenant, workflow));
        return it == checkpoints_.end() ? std::nullopt : std::optional(it->second);
    }
    JudgeStoreCommit InMemoryJudgeStore::compare_exchange(const JudgeCheckpoint &v, std::uint64_t expected)
    {
        std::string error;
        if (!valid_checkpoint(v, &error) || v.revision != expected + 1)
            return {JudgeStoreStatus::Invalid, 0, {}, error.empty() ? "revision must advance once" : error};
        std::lock_guard lock(mutex_);
        auto it = checkpoints_.find(key(v.metadata.identity.tenant_id, v.workflow_id));
        if (it == checkpoints_.end())
            return {JudgeStoreStatus::NotFound, 0, {}, {}};
        if (it->second.revision != expected)
            return {JudgeStoreStatus::RevisionConflict, it->second.revision, {}, {}};
        if (!immutable_match(it->second.checkpoint, v))
            return {JudgeStoreStatus::Invalid, expected, {}, "immutable input binding changed"};
        it->second = {v, v.revision};
        return {JudgeStoreStatus::Committed, v.revision, encode(v).at("canonical_digest").get<std::string>(), {}};
    }
    JudgeStoreCommit InMemoryJudgeStore::commit_report(const JudgeCheckpoint &v, std::uint64_t expected, const EvaluationReport &report)
    {
        std::string error;
        if (!valid_checkpoint(v, &error) || v.revision != expected + 1 || !terminal(v) ||
            !report_match(v, report))
            return {JudgeStoreStatus::Invalid, 0, {}, error.empty() ? "terminal checkpoint/report binding is invalid" : error};
        std::lock_guard lock(mutex_);
        const auto id = key(v.metadata.identity.tenant_id, v.workflow_id);
        auto it = checkpoints_.find(id);
        if (it == checkpoints_.end())
            return {JudgeStoreStatus::NotFound, 0, {}, {}};
        if (it->second.revision != expected)
            return {JudgeStoreStatus::RevisionConflict, it->second.revision, {}, {}};
        if (!immutable_match(it->second.checkpoint, v))
            return {JudgeStoreStatus::Invalid, expected, {}, "immutable input binding changed"};
        if (reports_.count(id))
            return {JudgeStoreStatus::AlreadyExists, expected, {}, {}};
        it->second = {v, v.revision};
        reports_[id] = {report, 1};
        return {JudgeStoreStatus::Committed, v.revision, encode(report).at("canonical_digest").get<std::string>(), {}};
    }
    std::optional<StoredEvaluationReport> InMemoryJudgeStore::load_report(std::string_view tenant, std::string_view workflow)
    {
        std::lock_guard lock(mutex_);
        auto it = reports_.find(key(tenant, workflow));
        return it == reports_.end() ? std::nullopt : std::optional(it->second);
    }

    SQLiteJudgeStore::SQLiteJudgeStore(std::string path, SQLiteJudgeStoreOptions options)
        : path_(std::move(path)), options_(options)
    {
        if (path_.empty())
            throw std::invalid_argument("judge store path must not be empty");
        std::filesystem::path file(path_);
        std::error_code ec;
        if (file.has_parent_path())
            std::filesystem::create_directories(file.parent_path(), ec);
        if (ec)
            throw std::runtime_error(ec.message());
        sqlite3 *opened = nullptr;
        if (sqlite3_open_v2(path_.c_str(), &opened, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
        {
            const std::string message = opened ? sqlite3_errmsg(opened) : "sqlite open failed";
            if (opened)
                sqlite3_close(opened);
            throw std::runtime_error(message);
        }
        db_ = opened;
        sqlite3_busy_timeout(opened, options_.busy_timeout_ms);
        sqlite::exec(opened, "PRAGMA journal_mode=WAL");
        sqlite::exec(opened, "PRAGMA synchronous=FULL");
        migrate();
#if !defined(_WIN32)
        if (options_.require_private_permissions)
        {
            std::filesystem::permissions(file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, ec);
            if (ec)
                throw std::runtime_error(ec.message());
        }
#endif
    }
    SQLiteJudgeStore::~SQLiteJudgeStore()
    {
        if (db_)
            sqlite3_close(sqlite::database(db_));
    }
    void SQLiteJudgeStore::migrate()
    {
        auto *db = sqlite::database(db_);
        sqlite::exec(db, "CREATE TABLE IF NOT EXISTS judge_schema_version(version INTEGER PRIMARY KEY,applied_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)");
        sqlite::exec(db, "INSERT OR IGNORE INTO judge_schema_version(version) VALUES(1)");
        sqlite::exec(db, "CREATE TABLE IF NOT EXISTS judge_checkpoints(tenant_id TEXT NOT NULL,workflow_id TEXT NOT NULL,task_id TEXT NOT NULL,revision INTEGER NOT NULL,state TEXT NOT NULL,checkpoint_json TEXT NOT NULL,checkpoint_digest TEXT NOT NULL,updated_at TEXT NOT NULL,PRIMARY KEY(tenant_id,workflow_id))");
        sqlite::exec(db, "CREATE TABLE IF NOT EXISTS evaluation_reports(tenant_id TEXT NOT NULL,workflow_id TEXT NOT NULL,revision INTEGER NOT NULL,report_json TEXT NOT NULL,report_digest TEXT NOT NULL,created_at TEXT NOT NULL,PRIMARY KEY(tenant_id,workflow_id))");
        sqlite::exec(db, "CREATE INDEX IF NOT EXISTS judge_task_idx ON judge_checkpoints(tenant_id,task_id,state)");
    }
    JudgeStoreCommit SQLiteJudgeStore::create(const JudgeCheckpoint &v)
    {
        std::string error;
        if (!valid_checkpoint(v, &error) || v.revision != 1)
            return {JudgeStoreStatus::Invalid, 0, {}, error.empty() ? "initial revision must be 1" : error};
        auto doc = encode(v);
        auto dg = doc.at("canonical_digest").get<std::string>();
        std::lock_guard lock(mutex_);
        auto *db = sqlite::database(db_);
        sqlite::Statement s(db, "INSERT INTO judge_checkpoints(tenant_id,workflow_id,task_id,revision,state,checkpoint_json,checkpoint_digest,updated_at) VALUES(?,?,?,?,?,?,?,?)");
        sqlite::bind_text(s.get(), 1, v.metadata.identity.tenant_id);
        sqlite::bind_text(s.get(), 2, v.workflow_id);
        sqlite::bind_text(s.get(), 3, v.metadata.identity.task_id);
        sqlite::bind_int64(s.get(), 4, v.revision);
        sqlite::bind_text(s.get(), 5, judge_workflow_state_name(v.state));
        sqlite::bind_text(s.get(), 6, doc.dump());
        sqlite::bind_text(s.get(), 7, dg);
        sqlite::bind_text(s.get(), 8, v.updated_at);
        int rc = sqlite::step(s.get());
        if (rc == SQLITE_CONSTRAINT)
            return {JudgeStoreStatus::AlreadyExists, 0, {}, {}};
        if (rc != SQLITE_DONE)
            return failure(db, rc);
        return {JudgeStoreStatus::Committed, v.revision, dg, {}};
    }
    std::optional<StoredJudgeCheckpoint> SQLiteJudgeStore::load(std::string_view tenant, std::string_view workflow)
    {
        std::lock_guard lock(mutex_);
        auto *db = sqlite::database(db_);
        sqlite::Statement s(db, "SELECT revision,checkpoint_json,checkpoint_digest FROM judge_checkpoints WHERE tenant_id=? AND workflow_id=?");
        sqlite::bind_text(s.get(), 1, tenant);
        sqlite::bind_text(s.get(), 2, workflow);
        int rc = sqlite::step(s.get());
        if (rc == SQLITE_DONE)
            return std::nullopt;
        if (rc != SQLITE_ROW)
            throw std::runtime_error(sqlite3_errmsg(db));
        auto rev = static_cast<std::uint64_t>(sqlite::column_int64(s.get(), 0));
        auto text = sqlite::column_text(s.get(), 1);
        auto dg = sqlite::column_text(s.get(), 2);
        auto value = decode_judge_checkpoint(nlohmann::json::parse(text));
        if (!value || value->revision != rev || encode(*value).at("canonical_digest").get<std::string>() != dg)
            throw std::runtime_error("stored judge checkpoint is corrupt");
        return StoredJudgeCheckpoint{std::move(*value), rev};
    }
    JudgeStoreCommit SQLiteJudgeStore::compare_exchange(const JudgeCheckpoint &v, std::uint64_t expected)
    {
        std::string error;
        if (!valid_checkpoint(v, &error) || v.revision != expected + 1)
            return {JudgeStoreStatus::Invalid, 0, {}, error.empty() ? "revision must advance once" : error};
        auto existing = load(v.metadata.identity.tenant_id, v.workflow_id);
        if (!existing)
            return {JudgeStoreStatus::NotFound, 0, {}, {}};
        if (existing->revision != expected)
            return {JudgeStoreStatus::RevisionConflict, existing->revision, {}, {}};
        if (!immutable_match(existing->checkpoint, v))
            return {JudgeStoreStatus::Invalid, expected, {}, "immutable input binding changed"};
        auto doc = encode(v);
        auto dg = doc.at("canonical_digest").get<std::string>();
        std::lock_guard lock(mutex_);
        auto *db = sqlite::database(db_);
        sqlite::Statement s(db, "UPDATE judge_checkpoints SET revision=?,state=?,checkpoint_json=?,checkpoint_digest=?,updated_at=? WHERE tenant_id=? AND workflow_id=? AND revision=?");
        sqlite::bind_int64(s.get(), 1, v.revision);
        sqlite::bind_text(s.get(), 2, judge_workflow_state_name(v.state));
        sqlite::bind_text(s.get(), 3, doc.dump());
        sqlite::bind_text(s.get(), 4, dg);
        sqlite::bind_text(s.get(), 5, v.updated_at);
        sqlite::bind_text(s.get(), 6, v.metadata.identity.tenant_id);
        sqlite::bind_text(s.get(), 7, v.workflow_id);
        sqlite::bind_int64(s.get(), 8, expected);
        int rc = sqlite::step(s.get());
        if (rc != SQLITE_DONE)
            return failure(db, rc);
        if (sqlite::changes(db) != 1)
            return {JudgeStoreStatus::RevisionConflict, expected, {}, {}};
        return {JudgeStoreStatus::Committed, v.revision, dg, {}};
    }
    JudgeStoreCommit SQLiteJudgeStore::commit_report(const JudgeCheckpoint &v, std::uint64_t expected, const EvaluationReport &report)
    {
        std::string error;
        if (!valid_checkpoint(v, &error) || v.revision != expected + 1 || !terminal(v) ||
            !report_match(v, report))
            return {JudgeStoreStatus::Invalid, 0, {}, error.empty() ? "terminal checkpoint/report binding is invalid" : error};
        auto cp = encode(v), rp = encode(report);
        auto cd = cp.at("canonical_digest").get<std::string>(), rd = rp.at("canonical_digest").get<std::string>();
        std::lock_guard lock(mutex_);
        auto *db = sqlite::database(db_);
        try
        {
            sqlite::exec(db, "BEGIN IMMEDIATE");
            sqlite::Statement q(db, "SELECT checkpoint_json FROM judge_checkpoints WHERE tenant_id=? AND workflow_id=? AND revision=?");
            sqlite::bind_text(q.get(), 1, v.metadata.identity.tenant_id);
            sqlite::bind_text(q.get(), 2, v.workflow_id);
            sqlite::bind_int64(q.get(), 3, expected);
            if (sqlite::step(q.get()) != SQLITE_ROW)
                throw std::runtime_error("judge checkpoint revision conflict");
            auto previous = decode_judge_checkpoint(nlohmann::json::parse(sqlite::column_text(q.get(), 0)));
            if (!previous || !immutable_match(*previous, v))
                throw std::runtime_error("immutable input binding changed");
            sqlite::Statement u(db, "UPDATE judge_checkpoints SET revision=?,state=?,checkpoint_json=?,checkpoint_digest=?,updated_at=? WHERE tenant_id=? AND workflow_id=? AND revision=?");
            sqlite::bind_int64(u.get(), 1, v.revision);
            sqlite::bind_text(u.get(), 2, judge_workflow_state_name(v.state));
            sqlite::bind_text(u.get(), 3, cp.dump());
            sqlite::bind_text(u.get(), 4, cd);
            sqlite::bind_text(u.get(), 5, v.updated_at);
            sqlite::bind_text(u.get(), 6, v.metadata.identity.tenant_id);
            sqlite::bind_text(u.get(), 7, v.workflow_id);
            sqlite::bind_int64(u.get(), 8, expected);
            if (sqlite::step(u.get()) != SQLITE_DONE || sqlite::changes(db) != 1)
                throw std::runtime_error("judge checkpoint revision conflict");
            sqlite::Statement ins(db, "INSERT INTO evaluation_reports(tenant_id,workflow_id,revision,report_json,report_digest,created_at) VALUES(?,?,?,?,?,?)");
            sqlite::bind_text(ins.get(), 1, v.metadata.identity.tenant_id);
            sqlite::bind_text(ins.get(), 2, v.workflow_id);
            sqlite::bind_int64(ins.get(), 3, 1);
            sqlite::bind_text(ins.get(), 4, rp.dump());
            sqlite::bind_text(ins.get(), 5, rd);
            sqlite::bind_text(ins.get(), 6, report.created_at);
            int rc = sqlite::step(ins.get());
            if (rc != SQLITE_DONE)
                throw std::runtime_error(sqlite3_errmsg(db));
            sqlite::exec(db, "COMMIT");
            return {JudgeStoreStatus::Committed, v.revision, rd, {}};
        }
        catch (const std::exception &e)
        {
            try
            {
                sqlite::exec(db, "ROLLBACK");
            }
            catch (...)
            {
            }
            return {JudgeStoreStatus::Error, 0, {}, e.what()};
        }
    }
    std::optional<StoredEvaluationReport> SQLiteJudgeStore::load_report(std::string_view tenant, std::string_view workflow)
    {
        std::lock_guard lock(mutex_);
        auto *db = sqlite::database(db_);
        sqlite::Statement s(db, "SELECT revision,report_json,report_digest FROM evaluation_reports WHERE tenant_id=? AND workflow_id=?");
        sqlite::bind_text(s.get(), 1, tenant);
        sqlite::bind_text(s.get(), 2, workflow);
        int rc = sqlite::step(s.get());
        if (rc == SQLITE_DONE)
            return std::nullopt;
        if (rc != SQLITE_ROW)
            throw std::runtime_error(sqlite3_errmsg(db));
        auto rev = static_cast<std::uint64_t>(sqlite::column_int64(s.get(), 0));
        auto value = decode_evaluation_report(nlohmann::json::parse(sqlite::column_text(s.get(), 1)));
        auto dg = sqlite::column_text(s.get(), 2);
        if (!value || encode(*value).at("canonical_digest").get<std::string>() != dg)
            throw std::runtime_error("stored evaluation report is corrupt");
        return StoredEvaluationReport{std::move(*value), rev};
    }

} // namespace agent_framework::eval
