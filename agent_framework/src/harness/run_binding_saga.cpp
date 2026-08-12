#include "agent/harness/run_binding_saga.hpp"

#include <filesystem>
#include <stdexcept>

#include <sqlite3.h>

#include "agent/contracts/contract.hpp"
#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::harness {
namespace {
namespace sqlite = internal::sqlite;

const char* state_name(RunBindingState state) {
    switch(state) {
        case RunBindingState::Prepared: return "prepared";
        case RunBindingState::Committed: return "committed";
        case RunBindingState::Reconciled: return "reconciled";
        case RunBindingState::ManualReview: return "manual_review";
    }
    return "manual_review";
}

RunBindingState parse_state(std::string_view value) {
    if(value == "prepared") return RunBindingState::Prepared;
    if(value == "committed") return RunBindingState::Committed;
    if(value == "reconciled") return RunBindingState::Reconciled;
    return RunBindingState::ManualReview;
}

std::string run_digest(const run::RunCheckpoint& checkpoint) {
    return contracts::canonical_digest(run::encode(checkpoint)).value_or("");
}
}

SQLiteRunHarnessSaga::SQLiteRunHarnessSaga(std::string path, run::RunStore& runs,
                                           HarnessStore& harnesses)
    : runs_(runs), harnesses_(harnesses) {
    if(path.empty()) throw std::invalid_argument("saga journal path must not be empty");
    const std::filesystem::path file(path);
    std::error_code error;
    if(file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), error);
    if(error) throw std::runtime_error(error.message());
    sqlite3* opened = nullptr;
    if(sqlite3_open_v2(path.c_str(), &opened,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        const std::string message = opened ? sqlite3_errmsg(opened) : "sqlite open failed";
        if(opened) sqlite3_close(opened);
        throw std::runtime_error(message);
    }
    db_ = opened;
    sqlite3_busy_timeout(opened, 3000);
    sqlite::exec(opened, "PRAGMA journal_mode=WAL");
    sqlite::exec(opened, "PRAGMA synchronous=FULL");
    migrate();
}

SQLiteRunHarnessSaga::~SQLiteRunHarnessSaga() {
    if(db_) sqlite3_close(sqlite::database(db_));
}

void SQLiteRunHarnessSaga::migrate() {
    auto* db = sqlite::database(db_);
    sqlite::exec(db, "CREATE TABLE IF NOT EXISTS phase4_run_harness_bindings("
        "binding_id TEXT PRIMARY KEY,tenant_id TEXT NOT NULL,run_id TEXT NOT NULL,"
        "harness_id TEXT NOT NULL,stage INTEGER NOT NULL,run_revision INTEGER NOT NULL,"
        "run_digest TEXT NOT NULL,harness_revision INTEGER NOT NULL,harness_digest TEXT NOT NULL,"
        "state TEXT NOT NULL,diagnostic TEXT NOT NULL DEFAULT '',updated_at TEXT NOT NULL "
        "DEFAULT CURRENT_TIMESTAMP)");
    sqlite::exec(db, "CREATE INDEX IF NOT EXISTS phase4_run_harness_unresolved_idx ON "
        "phase4_run_harness_bindings(state,updated_at)");
}

RunBindingResult SQLiteRunHarnessSaga::prepare(const RunHarnessBinding& binding) {
    if(binding.binding_id.empty() || binding.tenant_id.empty() || binding.run_id.empty() ||
       binding.harness_id.empty() || binding.run_revision == 0 ||
       binding.harness_revision == 0 || binding.run_digest.empty() ||
       binding.harness_digest.empty())
        return {false, RunBindingState::Prepared, "binding identity and pins are required"};
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    sqlite::Statement statement(db, "INSERT INTO phase4_run_harness_bindings("
        "binding_id,tenant_id,run_id,harness_id,stage,run_revision,run_digest,harness_revision,"
        "harness_digest,state,diagnostic) VALUES(?,?,?,?,?,?,?,?,?,?,?)");
    sqlite::bind_text(statement.get(), 1, binding.binding_id);
    sqlite::bind_text(statement.get(), 2, binding.tenant_id);
    sqlite::bind_text(statement.get(), 3, binding.run_id);
    sqlite::bind_text(statement.get(), 4, binding.harness_id);
    sqlite::bind_int64(statement.get(), 5, static_cast<std::int64_t>(binding.stage));
    sqlite::bind_int64(statement.get(), 6, binding.run_revision);
    sqlite::bind_text(statement.get(), 7, binding.run_digest);
    sqlite::bind_int64(statement.get(), 8, binding.harness_revision);
    sqlite::bind_text(statement.get(), 9, binding.harness_digest);
    sqlite::bind_text(statement.get(), 10, state_name(RunBindingState::Prepared));
    sqlite::bind_text(statement.get(), 11, binding.diagnostic);
    const int rc = sqlite::step(statement.get());
    if(rc == SQLITE_DONE) return {true, RunBindingState::Prepared, {}};
    if(rc == SQLITE_CONSTRAINT) return {false, RunBindingState::Prepared, "binding already exists"};
    return {false, RunBindingState::Prepared, sqlite3_errmsg(db)};
}

std::optional<RunHarnessBinding> SQLiteRunHarnessSaga::load(std::string_view binding_id) {
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    sqlite::Statement statement(db, "SELECT tenant_id,run_id,harness_id,stage,run_revision,"
        "run_digest,harness_revision,harness_digest,state,diagnostic FROM "
        "phase4_run_harness_bindings WHERE binding_id=?");
    sqlite::bind_text(statement.get(), 1, binding_id);
    const int rc = sqlite::step(statement.get());
    if(rc == SQLITE_DONE) return std::nullopt;
    if(rc != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(db));
    RunHarnessBinding value;
    value.binding_id = std::string(binding_id);
    value.tenant_id = sqlite::column_text(statement.get(), 0);
    value.run_id = sqlite::column_text(statement.get(), 1);
    value.harness_id = sqlite::column_text(statement.get(), 2);
    value.stage = static_cast<HarnessStage>(sqlite::column_int64(statement.get(), 3));
    value.run_revision = static_cast<std::uint64_t>(sqlite::column_int64(statement.get(), 4));
    value.run_digest = sqlite::column_text(statement.get(), 5);
    value.harness_revision = static_cast<std::uint64_t>(sqlite::column_int64(statement.get(), 6));
    value.harness_digest = sqlite::column_text(statement.get(), 7);
    value.state = parse_state(sqlite::column_text(statement.get(), 8));
    value.diagnostic = sqlite::column_text(statement.get(), 9);
    return value;
}

RunBindingResult SQLiteRunHarnessSaga::advance(std::string_view binding_id,
                                               RunBindingState expected,
                                               RunBindingState next,
                                               std::string_view diagnostic) {
    std::lock_guard lock(mutex_);
    auto* db = sqlite::database(db_);
    sqlite::Statement statement(db, "UPDATE phase4_run_harness_bindings SET state=?,diagnostic=?,"
        "updated_at=CURRENT_TIMESTAMP WHERE binding_id=? AND state=?");
    sqlite::bind_text(statement.get(), 1, state_name(next));
    sqlite::bind_text(statement.get(), 2, diagnostic);
    sqlite::bind_text(statement.get(), 3, binding_id);
    sqlite::bind_text(statement.get(), 4, state_name(expected));
    const int rc = sqlite::step(statement.get());
    if(rc != SQLITE_DONE) return {false, expected, sqlite3_errmsg(db)};
    if(sqlite::changes(db) != 1) return {false, expected, "binding state conflict"};
    return {true, next, std::string(diagnostic)};
}

RunBindingResult SQLiteRunHarnessSaga::commit(std::string_view binding_id) {
    const auto binding = load(binding_id);
    if(!binding) return {false, RunBindingState::Prepared, "binding not found"};
    if(binding->state == RunBindingState::Committed ||
       binding->state == RunBindingState::Reconciled) return {true, binding->state, {}};
    if(binding->state != RunBindingState::Prepared)
        return {false, binding->state, binding->diagnostic};
    const auto run = runs_.load(binding->run_id);
    const auto harness = harnesses_.load(binding->tenant_id, binding->harness_id);
    const bool matches = run && harness && run->revision == binding->run_revision &&
        harness->revision == binding->harness_revision &&
        run_digest(run->checkpoint) == binding->run_digest &&
        harness->digest == binding->harness_digest;
    if(!matches)
        return advance(binding_id, RunBindingState::Prepared,
                       RunBindingState::ManualReview, "revision or digest pin mismatch");
    return advance(binding_id, RunBindingState::Prepared, RunBindingState::Committed, "");
}

RunBindingResult SQLiteRunHarnessSaga::reconcile(std::string_view binding_id) {
    auto result = commit(binding_id);
    if(!result.committed || result.state == RunBindingState::ManualReview) return result;
    if(result.state == RunBindingState::Reconciled) return result;
    return advance(binding_id, RunBindingState::Committed, RunBindingState::Reconciled, "");
}

std::vector<RunHarnessBinding> SQLiteRunHarnessSaga::list_unresolved(std::size_t limit) {
    std::vector<std::string> ids;
    {
        std::lock_guard lock(mutex_);
        auto* db = sqlite::database(db_);
        sqlite::Statement statement(db, "SELECT binding_id FROM phase4_run_harness_bindings "
            "WHERE state IN ('prepared','committed','manual_review') ORDER BY updated_at LIMIT ?");
        sqlite::bind_int64(statement.get(), 1, static_cast<std::int64_t>(limit));
        while(sqlite::step(statement.get()) == SQLITE_ROW)
            ids.push_back(sqlite::column_text(statement.get(), 0));
    }
    std::vector<RunHarnessBinding> result;
    for(const auto& id : ids) if(auto value = load(id)) result.push_back(std::move(*value));
    return result;
}

}  // namespace agent_framework::harness
