#include "agent/conversation/task_registry.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <stdexcept>

#include <sqlite3.h>

#include "agent/contracts/contract.hpp"
#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::conversation {
namespace {
namespace sql = agent_framework::internal::sqlite;
using json = nlohmann::json;

std::string stamp() {
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string canonical(json value) {
    value.erase("canonical_digest");
    return contracts::canonical_digest(value).value_or("");
}

std::string normalized(std::string_view input) {
    std::string value(input);
    while(!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while(!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

void bind_identity(sqlite3_stmt* statement, const ConversationIdentity& identity) {
    sql::bind_text(statement, 1, identity.tenant_id);
    sql::bind_text(statement, 2, identity.conversation_id);
}

PersistentTask task_from(sqlite3_stmt* row, const ConversationIdentity& identity) {
    PersistentTask value;
    value.identity = identity;
    value.task_id = sql::column_text(row, 0);
    value.root_turn_id = sql::column_text(row, 1);
    value.current_turn_id = sql::column_text(row, 2);
    value.current_run_id = sql::column_text(row, 3);
    value.parent_task_id = sql::column_text(row, 4);
    value.state = *task_lifecycle_state(sql::column_text(row, 5));
    value.closure_state = sql::column_text(row, 6);
    value.revision = sql::column_uint64(row, 7);
    value.requirement_revision = sql::column_uint64(row, 8);
    value.plan_revision = sql::column_uint64(row, 9);
    value.created_at = sql::column_text(row, 10);
    value.updated_at = sql::column_text(row, 11);
    value.digest = sql::column_text(row, 12);
    if(canonical(encode(value)) != value.digest)
        throw std::runtime_error("stored task digest mismatch");
    return value;
}

void append_event(sqlite3* db, const PersistentTask& task, std::string_view type,
                  const json& payload) {
    sql::Statement tail(db,
        "SELECT sequence,event_digest FROM conversation_task_events "
        "WHERE tenant=? AND conversation=? AND task_id=? ORDER BY sequence DESC LIMIT 1");
    bind_identity(tail.get(), task.identity);
    sql::bind_text(tail.get(), 3, task.task_id);
    std::uint64_t sequence = 1;
    std::string previous;
    if(sql::step(tail.get()) == SQLITE_ROW) {
        sequence = sql::column_uint64(tail.get(), 0) + 1;
        previous = sql::column_text(tail.get(), 1);
    }
    const auto payload_digest = canonical(payload);
    const auto event_digest = canonical({{"task_id", task.task_id},
        {"sequence", sequence}, {"task_revision", task.revision},
        {"event_type", type}, {"payload_digest", payload_digest},
        {"previous_digest", previous}});
    sql::Statement insert(db,
        "INSERT INTO conversation_task_events VALUES(?,?,?,?,?,?,?,?,?,?)");
    bind_identity(insert.get(), task.identity);
    sql::bind_text(insert.get(), 3, task.task_id);
    sql::bind_uint64(insert.get(), 4, sequence);
    sql::bind_uint64(insert.get(), 5, task.revision);
    sql::bind_text(insert.get(), 6, type);
    sql::bind_text(insert.get(), 7, payload.dump());
    sql::bind_text(insert.get(), 8, payload_digest);
    sql::bind_text(insert.get(), 9, previous);
    sql::bind_text(insert.get(), 10, event_digest);
    if(sql::step(insert.get()) != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(db));
}

void insert_requirement(sqlite3* db, TaskRequirementRevision value) {
    if(value.created_at.empty()) value.created_at = stamp();
    value.digest = canonical(encode(value));
    sql::Statement insert(db,
        "INSERT INTO task_requirement_revisions VALUES(?,?,?,?,?,?,?,?,?,?)");
    bind_identity(insert.get(), value.identity);
    sql::bind_text(insert.get(), 3, value.task_id);
    sql::bind_uint64(insert.get(), 4, value.revision);
    sql::bind_text(insert.get(), 5, name(value.intent));
    sql::bind_text(insert.get(), 6, value.turn_id);
    sql::bind_text(insert.get(), 7, value.content);
    sql::bind_text(insert.get(), 8, value.previous_digest);
    sql::bind_text(insert.get(), 9, value.digest);
    sql::bind_text(insert.get(), 10, value.created_at);
    if(sql::step(insert.get()) != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(db));
}

void insert_link(sqlite3* db, TurnTaskLink value) {
    if(value.created_at.empty()) value.created_at = stamp();
    sql::Statement insert(db,
        "INSERT INTO task_turn_links VALUES(?,?,?,?,?,?,?,?)");
    bind_identity(insert.get(), value.identity);
    sql::bind_text(insert.get(), 3, value.turn_id);
    sql::bind_text(insert.get(), 4, value.task_id);
    sql::bind_text(insert.get(), 5, value.run_id);
    sql::bind_uint64(insert.get(), 6, value.requirement_revision);
    sql::bind_text(insert.get(), 7, name(value.intent));
    sql::bind_text(insert.get(), 8, value.created_at);
    if(sql::step(insert.get()) != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(db));
}

void insert_run_link(sqlite3* db, TaskRunLink value) {
    const auto now = stamp();
    if(value.created_at.empty()) value.created_at = now;
    if(value.updated_at.empty()) value.updated_at = now;
    sql::Statement insert(db,
        "INSERT INTO task_run_links VALUES(?,?,?,?,?,?,?,?,?)");
    bind_identity(insert.get(), value.identity);
    sql::bind_text(insert.get(), 3, value.task_id);
    sql::bind_text(insert.get(), 4, value.run_id);
    sql::bind_uint64(insert.get(), 5, value.requirement_revision);
    sql::bind_uint64(insert.get(), 6, value.plan_revision);
    sql::bind_text(insert.get(), 7, value.state);
    sql::bind_text(insert.get(), 8, value.created_at);
    sql::bind_text(insert.get(), 9, value.updated_at);
    if(sql::step(insert.get()) != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(db));
}
}  // namespace

std::string_view name(TaskLifecycleState value) {
    static constexpr std::array<std::string_view, 8> names{
        "active", "awaiting_input", "awaiting_approval", "suspended",
        "closing", "closed", "failed", "cancelled"};
    const auto index = static_cast<std::size_t>(value);
    return index < names.size() ? names[index] : "unknown";
}

std::string_view name(TaskInputIntent value) {
    static constexpr std::array<std::string_view, 8> names{
        "initial_request", "continue_task", "amend_requirements",
        "status_query", "cancel_task", "suspend_task", "replan_task", "start_new_task"};
    const auto index = static_cast<std::size_t>(value);
    return index < names.size() ? names[index] : "unknown";
}

std::optional<TaskLifecycleState> task_lifecycle_state(std::string_view text) {
    for(std::size_t index = 0; index < 8; ++index) {
        const auto value = static_cast<TaskLifecycleState>(index);
        if(name(value) == text) return value;
    }
    return std::nullopt;
}

std::optional<TaskInputIntent> task_input_intent(std::string_view text) {
    for(std::size_t index = 0; index < 8; ++index) {
        const auto value = static_cast<TaskInputIntent>(index);
        if(name(value) == text) return value;
    }
    return std::nullopt;
}

TaskInputIntent classify_task_input(std::string_view input, bool has_active_task) {
    const auto value = normalized(input);
    if(value == "/status" || value == "状态" || value == "进度" ||
       value == "现在怎么样" || value == "现在结果怎么样了")
        return TaskInputIntent::StatusQuery;
    if(value == "/cancel" || value == "取消" || value == "取消任务")
        return TaskInputIntent::CancelTask;
    if(value == "/stop" || value == "/suspend" || value == "停止任务" ||
       value == "暂停" || value == "先停一下")
        return TaskInputIntent::SuspendTask;
    if(value == "/replan" || value == "重新规划" || value == "换方案")
        return TaskInputIntent::ReplanTask;
    if(value.rfind("/new ", 0) == 0 || value == "/new")
        return TaskInputIntent::StartNewTask;
    if(value == "继续" || value == "请继续" || value == "需要" ||
       value == "继续下一步" || value == "执行到任务完成")
        return has_active_task ? TaskInputIntent::ContinueTask
                               : TaskInputIntent::InitialRequest;
    return has_active_task ? TaskInputIntent::AmendRequirements
                           : TaskInputIntent::InitialRequest;
}

json encode(const PersistentTask& value) {
    return {{"schema", "agent.persistent_task/v1"},
        {"identity", {{"tenant_id", value.identity.tenant_id},
                      {"conversation_id", value.identity.conversation_id}}},
        {"task_id", value.task_id}, {"root_turn_id", value.root_turn_id},
        {"current_turn_id", value.current_turn_id},
        {"current_run_id", value.current_run_id},
        {"parent_task_id", value.parent_task_id}, {"state", name(value.state)},
        {"closure_state", value.closure_state}, {"revision", value.revision},
        {"requirement_revision", value.requirement_revision},
        {"plan_revision", value.plan_revision}, {"created_at", value.created_at},
        {"updated_at", value.updated_at}};
}

json encode(const TaskRequirementRevision& value) {
    return {{"schema", "agent.task_requirement_revision/v1"},
        {"identity", {{"tenant_id", value.identity.tenant_id},
                      {"conversation_id", value.identity.conversation_id}}},
        {"task_id", value.task_id}, {"revision", value.revision},
        {"intent", name(value.intent)}, {"turn_id", value.turn_id},
        {"content", value.content}, {"previous_digest", value.previous_digest},
        {"created_at", value.created_at}};
}

SQLiteTaskRegistry::SQLiteTaskRegistry(std::string database_path)
    : path_(std::move(database_path)) {
    if(path_.empty()) throw std::invalid_argument("task registry path required");
    const std::filesystem::path file(path_);
    std::error_code error;
    if(file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), error);
    sqlite3* opened = nullptr;
    if(error || sqlite3_open_v2(path_.c_str(), &opened,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr) != SQLITE_OK) {
        const std::string message = opened ? sqlite3_errmsg(opened) : error.message();
        if(opened) sqlite3_close(opened);
        throw std::runtime_error(message);
    }
    db_ = opened;
    sqlite3_busy_timeout(opened, 3000);
    sql::exec(opened, "PRAGMA journal_mode=WAL");
    sql::exec(opened, "PRAGMA synchronous=FULL");
    sql::exec(opened, "PRAGMA foreign_keys=ON");
    migrate();
}

SQLiteTaskRegistry::~SQLiteTaskRegistry() {
    if(db_) sqlite3_close(sql::database(db_));
}

void SQLiteTaskRegistry::migrate() {
    auto* db = sql::database(db_);
    sql::exec(db, "CREATE TABLE IF NOT EXISTS conversation_task_schema_version("
                  "version INTEGER PRIMARY KEY,applied_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)");
    sql::exec(db, "INSERT OR IGNORE INTO conversation_task_schema_version(version) VALUES(1)");
    sql::exec(db, "INSERT OR IGNORE INTO conversation_task_schema_version(version) VALUES(2)");
    sql::exec(db, "CREATE TABLE IF NOT EXISTS conversation_tasks("
        "tenant TEXT NOT NULL,conversation TEXT NOT NULL,task_id TEXT NOT NULL,"
        "root_turn_id TEXT NOT NULL,current_turn_id TEXT NOT NULL,current_run_id TEXT NOT NULL,"
        "parent_task_id TEXT NOT NULL,state TEXT NOT NULL,closure_state TEXT NOT NULL,"
        "revision INTEGER NOT NULL,requirement_revision INTEGER NOT NULL,plan_revision INTEGER NOT NULL,"
        "created_at TEXT NOT NULL,updated_at TEXT NOT NULL,digest TEXT NOT NULL,"
        "PRIMARY KEY(tenant,conversation,task_id))");
    sql::exec(db, "CREATE TABLE IF NOT EXISTS conversation_active_tasks("
        "tenant TEXT NOT NULL,conversation TEXT NOT NULL,task_id TEXT NOT NULL,"
        "task_revision INTEGER NOT NULL,updated_at TEXT NOT NULL,"
        "PRIMARY KEY(tenant,conversation))");
    sql::exec(db, "CREATE TABLE IF NOT EXISTS task_requirement_revisions("
        "tenant TEXT NOT NULL,conversation TEXT NOT NULL,task_id TEXT NOT NULL,"
        "revision INTEGER NOT NULL,intent TEXT NOT NULL,turn_id TEXT NOT NULL,"
        "content TEXT NOT NULL,previous_digest TEXT NOT NULL,digest TEXT NOT NULL,"
        "created_at TEXT NOT NULL,PRIMARY KEY(tenant,conversation,task_id,revision))");
    sql::exec(db, "CREATE TABLE IF NOT EXISTS task_turn_links("
        "tenant TEXT NOT NULL,conversation TEXT NOT NULL,turn_id TEXT NOT NULL,"
        "task_id TEXT NOT NULL,run_id TEXT NOT NULL,requirement_revision INTEGER NOT NULL,"
        "intent TEXT NOT NULL,created_at TEXT NOT NULL,"
        "PRIMARY KEY(tenant,conversation,turn_id))");
    sql::exec(db, "CREATE TABLE IF NOT EXISTS task_run_links("
        "tenant TEXT NOT NULL,conversation TEXT NOT NULL,task_id TEXT NOT NULL,"
        "run_id TEXT NOT NULL,requirement_revision INTEGER NOT NULL,"
        "plan_revision INTEGER NOT NULL,state TEXT NOT NULL,created_at TEXT NOT NULL,"
        "updated_at TEXT NOT NULL,PRIMARY KEY(tenant,conversation,task_id,run_id))");
    sql::exec(db, "CREATE TABLE IF NOT EXISTS conversation_task_events("
        "tenant TEXT NOT NULL,conversation TEXT NOT NULL,task_id TEXT NOT NULL,"
        "sequence INTEGER NOT NULL,task_revision INTEGER NOT NULL,event_type TEXT NOT NULL,"
        "payload_json TEXT NOT NULL,payload_digest TEXT NOT NULL,"
        "previous_digest TEXT NOT NULL,event_digest TEXT NOT NULL,"
        "PRIMARY KEY(tenant,conversation,task_id,sequence))");
}

TaskMutationResult SQLiteTaskRegistry::create(
    PersistentTask task, TaskRequirementRevision requirement, TurnTaskLink link) {
    if(task.identity.tenant_id.empty() || task.identity.conversation_id.empty() ||
       task.task_id.empty() || task.root_turn_id.empty() ||
       task.current_run_id.empty() || requirement.content.empty())
        return {false, 0, "task_create_contract_invalid"};
    std::lock_guard lock(mutex_);
    auto* db = sql::database(db_);
    try {
        sql::Transaction transaction(db);
        const auto now = stamp();
        if(task.created_at.empty()) task.created_at = now;
        if(task.updated_at.empty()) task.updated_at = now;
        task.revision = 1;
        task.requirement_revision = 1;
        task.digest = canonical(encode(task));
        sql::Statement insert(db,
            "INSERT INTO conversation_tasks VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        bind_identity(insert.get(), task.identity);
        sql::bind_text(insert.get(), 3, task.task_id);
        sql::bind_text(insert.get(), 4, task.root_turn_id);
        sql::bind_text(insert.get(), 5, task.current_turn_id);
        sql::bind_text(insert.get(), 6, task.current_run_id);
        sql::bind_text(insert.get(), 7, task.parent_task_id);
        sql::bind_text(insert.get(), 8, name(task.state));
        sql::bind_text(insert.get(), 9, task.closure_state);
        sql::bind_uint64(insert.get(), 10, task.revision);
        sql::bind_uint64(insert.get(), 11, task.requirement_revision);
        sql::bind_uint64(insert.get(), 12, task.plan_revision);
        sql::bind_text(insert.get(), 13, task.created_at);
        sql::bind_text(insert.get(), 14, task.updated_at);
        sql::bind_text(insert.get(), 15, task.digest);
        if(sql::step(insert.get()) != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(db));
        requirement.identity = task.identity;
        requirement.task_id = task.task_id;
        requirement.revision = 1;
        requirement.intent = requirement.intent == TaskInputIntent::StartNewTask
            ? TaskInputIntent::StartNewTask : TaskInputIntent::InitialRequest;
        insert_requirement(db, requirement);
        link.identity = task.identity;
        link.task_id = task.task_id;
        link.run_id = task.current_run_id;
        link.requirement_revision = 1;
        insert_link(db, link);
        TaskRunLink initial_run;
        initial_run.identity = task.identity;
        initial_run.task_id = task.task_id;
        initial_run.run_id = task.current_run_id;
        initial_run.requirement_revision = 1;
        initial_run.plan_revision = task.plan_revision;
        insert_run_link(db, initial_run);
        sql::Statement active(db,
            "INSERT INTO conversation_active_tasks VALUES(?,?,?,?,?) "
            "ON CONFLICT(tenant,conversation) DO UPDATE SET "
            "task_id=excluded.task_id,task_revision=excluded.task_revision,"
            "updated_at=excluded.updated_at");
        bind_identity(active.get(), task.identity);
        sql::bind_text(active.get(), 3, task.task_id);
        sql::bind_uint64(active.get(), 4, task.revision);
        sql::bind_text(active.get(), 5, task.updated_at);
        if(sql::step(active.get()) != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(db));
        append_event(db, task, "task_created",
            {{"turn_id", task.root_turn_id}, {"run_id", task.current_run_id},
             {"requirement_digest", canonical(encode(requirement))}});
        transaction.commit();
        return {true, task.revision, {}};
    } catch(const std::exception& error) {
        return {false, 0, error.what()};
    }
}

TaskMutationResult SQLiteTaskRegistry::attach_turn(
    const TurnTaskLink& link, std::uint64_t expected) {
    if(link.task_id.empty() || link.turn_id.empty() || link.run_id.empty())
        return {false, 0, "task_turn_link_contract_invalid"};
    std::lock_guard lock(mutex_);
    auto* db = sql::database(db_);
    try {
        sql::Transaction transaction(db);
        sql::Statement old(db,
            "SELECT task_id,root_turn_id,current_turn_id,current_run_id,parent_task_id,"
            "state,closure_state,revision,requirement_revision,plan_revision,"
            "created_at,updated_at,digest FROM conversation_tasks "
            "WHERE tenant=? AND conversation=? AND task_id=?");
        bind_identity(old.get(), link.identity);
        sql::bind_text(old.get(), 3, link.task_id);
        if(sql::step(old.get()) != SQLITE_ROW) return {false, 0, "task_not_found"};
        auto task = task_from(old.get(), link.identity);
        if(task.revision != expected) return {false, task.revision, "task_revision_conflict"};
        auto stored_link = link;
        stored_link.requirement_revision = task.requirement_revision;
        insert_link(db, stored_link);
        ++task.revision;
        task.current_turn_id = link.turn_id;
        task.current_run_id = link.run_id;
        task.updated_at = stamp();
        task.digest = canonical(encode(task));
        sql::Statement update(db,
            "UPDATE conversation_tasks SET current_turn_id=?,current_run_id=?,revision=?,"
            "updated_at=?,digest=? WHERE tenant=? AND conversation=? AND task_id=? AND revision=?");
        sql::bind_text(update.get(), 1, task.current_turn_id);
        sql::bind_text(update.get(), 2, task.current_run_id);
        sql::bind_uint64(update.get(), 3, task.revision);
        sql::bind_text(update.get(), 4, task.updated_at);
        sql::bind_text(update.get(), 5, task.digest);
        sql::bind_text(update.get(), 6, task.identity.tenant_id);
        sql::bind_text(update.get(), 7, task.identity.conversation_id);
        sql::bind_text(update.get(), 8, task.task_id);
        sql::bind_uint64(update.get(), 9, expected);
        if(sql::step(update.get()) != SQLITE_DONE || sql::changes(db) != 1)
            throw std::runtime_error("task_revision_conflict");
        sql::Statement active(db,
            "UPDATE conversation_active_tasks SET task_revision=?,updated_at=? "
            "WHERE tenant=? AND conversation=? AND task_id=?");
        sql::bind_uint64(active.get(), 1, task.revision);
        sql::bind_text(active.get(), 2, task.updated_at);
        sql::bind_text(active.get(), 3, task.identity.tenant_id);
        sql::bind_text(active.get(), 4, task.identity.conversation_id);
        sql::bind_text(active.get(), 5, task.task_id);
        if(sql::step(active.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
        append_event(db, task, "turn_attached",
            {{"turn_id", link.turn_id}, {"run_id", link.run_id},
             {"intent", name(link.intent)}});
        transaction.commit();
        return {true, task.revision, {}};
    } catch(const std::exception& error) { return {false, 0, error.what()}; }
}

TaskMutationResult SQLiteTaskRegistry::bind_run(
    const TaskRunLink& link, std::uint64_t expected) {
    if(link.task_id.empty() || link.run_id.empty())
        return {false, 0, "task_run_link_contract_invalid"};
    std::lock_guard lock(mutex_);
    auto* db = sql::database(db_);
    try {
        sql::Transaction transaction(db);
        sql::Statement old(db,
            "SELECT task_id,root_turn_id,current_turn_id,current_run_id,parent_task_id,"
            "state,closure_state,revision,requirement_revision,plan_revision,"
            "created_at,updated_at,digest FROM conversation_tasks "
            "WHERE tenant=? AND conversation=? AND task_id=?");
        bind_identity(old.get(), link.identity); sql::bind_text(old.get(), 3, link.task_id);
        if(sql::step(old.get()) != SQLITE_ROW) return {false, 0, "task_not_found"};
        auto task = task_from(old.get(), link.identity);
        if(task.revision != expected) return {false, task.revision, "task_revision_conflict"};
        auto stored = link;
        stored.requirement_revision = task.requirement_revision;
        insert_run_link(db, stored);
        ++task.revision; task.current_run_id = link.run_id;
        task.plan_revision = std::max(task.plan_revision, link.plan_revision);
        task.updated_at = stamp(); task.digest = canonical(encode(task));
        sql::Statement update(db,
            "UPDATE conversation_tasks SET current_run_id=?,revision=?,plan_revision=?,"
            "updated_at=?,digest=? WHERE tenant=? AND conversation=? AND task_id=? AND revision=?");
        sql::bind_text(update.get(),1,task.current_run_id); sql::bind_uint64(update.get(),2,task.revision);
        sql::bind_uint64(update.get(),3,task.plan_revision); sql::bind_text(update.get(),4,task.updated_at);
        sql::bind_text(update.get(),5,task.digest); sql::bind_text(update.get(),6,task.identity.tenant_id);
        sql::bind_text(update.get(),7,task.identity.conversation_id); sql::bind_text(update.get(),8,task.task_id);
        sql::bind_uint64(update.get(),9,expected);
        if(sql::step(update.get()) != SQLITE_DONE || sql::changes(db) != 1)
            throw std::runtime_error("task_revision_conflict");
        sql::Statement active(db,"UPDATE conversation_active_tasks SET task_revision=?,updated_at=? "
            "WHERE tenant=? AND conversation=? AND task_id=?");
        sql::bind_uint64(active.get(),1,task.revision); sql::bind_text(active.get(),2,task.updated_at);
        sql::bind_text(active.get(),3,task.identity.tenant_id); sql::bind_text(active.get(),4,task.identity.conversation_id);
        sql::bind_text(active.get(),5,task.task_id); sql::step(active.get());
        append_event(db, task, "run_bound", {{"run_id",link.run_id},
            {"plan_revision",link.plan_revision},{"state",link.state}});
        transaction.commit(); return {true,task.revision,{}};
    } catch(const std::exception& error) { return {false,0,error.what()}; }
}

std::optional<PersistentTask> SQLiteTaskRegistry::load(
    const ConversationIdentity& identity, std::string_view task_id) {
    std::lock_guard lock(mutex_);
    auto* db = sql::database(db_);
    sql::Statement query(db,
        "SELECT task_id,root_turn_id,current_turn_id,current_run_id,parent_task_id,"
        "state,closure_state,revision,requirement_revision,plan_revision,"
        "created_at,updated_at,digest FROM conversation_tasks "
        "WHERE tenant=? AND conversation=? AND task_id=?");
    bind_identity(query.get(), identity);
    sql::bind_text(query.get(), 3, task_id);
    if(sql::step(query.get()) != SQLITE_ROW) return std::nullopt;
    return task_from(query.get(), identity);
}

std::optional<PersistentTask> SQLiteTaskRegistry::active(
    const ConversationIdentity& identity) {
    std::lock_guard lock(mutex_);
    auto* db = sql::database(db_);
    sql::Statement query(db,
        "SELECT t.task_id,t.root_turn_id,t.current_turn_id,t.current_run_id,"
        "t.parent_task_id,t.state,t.closure_state,t.revision,t.requirement_revision,"
        "t.plan_revision,t.created_at,t.updated_at,t.digest "
        "FROM conversation_active_tasks a JOIN conversation_tasks t "
        "ON t.tenant=a.tenant AND t.conversation=a.conversation AND t.task_id=a.task_id "
        "WHERE a.tenant=? AND a.conversation=?");
    bind_identity(query.get(), identity);
    if(sql::step(query.get()) != SQLITE_ROW) return std::nullopt;
    auto value = task_from(query.get(), identity);
    if(value.state == TaskLifecycleState::Closed ||
       value.state == TaskLifecycleState::Failed ||
       value.state == TaskLifecycleState::Cancelled) return std::nullopt;
    return value;
}

TaskMutationResult SQLiteTaskRegistry::append_requirement(
    const TaskRequirementRevision& input, const TurnTaskLink& input_link,
    std::uint64_t expected, std::string_view run_id) {
    if(input.task_id.empty() || input.turn_id.empty() || input.content.empty() ||
       run_id.empty()) return {false, 0, "task_requirement_contract_invalid"};
    std::lock_guard lock(mutex_);
    auto* db = sql::database(db_);
    try {
        sql::Transaction transaction(db);
        sql::Statement old(db,
            "SELECT task_id,root_turn_id,current_turn_id,current_run_id,parent_task_id,"
            "state,closure_state,revision,requirement_revision,plan_revision,"
            "created_at,updated_at,digest FROM conversation_tasks "
            "WHERE tenant=? AND conversation=? AND task_id=?");
        bind_identity(old.get(), input.identity);
        sql::bind_text(old.get(), 3, input.task_id);
        if(sql::step(old.get()) != SQLITE_ROW) return {false, 0, "task_not_found"};
        auto task = task_from(old.get(), input.identity);
        if(task.revision != expected) return {false, task.revision, "task_revision_conflict"};
        TaskRequirementRevision requirement = input;
        requirement.revision = task.requirement_revision + 1;
        // Obtain the previous digest directly while already inside the transaction.
        sql::Statement previous(db,
            "SELECT digest FROM task_requirement_revisions WHERE tenant=? AND conversation=? "
            "AND task_id=? ORDER BY revision DESC LIMIT 1");
        bind_identity(previous.get(), input.identity);
        sql::bind_text(previous.get(), 3, input.task_id);
        if(sql::step(previous.get()) == SQLITE_ROW)
            requirement.previous_digest = sql::column_text(previous.get(), 0);
        insert_requirement(db, requirement);
        TurnTaskLink link = input_link;
        link.requirement_revision = requirement.revision;
        link.run_id = std::string(run_id);
        insert_link(db, link);
        ++task.revision;
        task.requirement_revision = requirement.revision;
        task.current_turn_id = input.turn_id;
        task.current_run_id = std::string(run_id);
        task.updated_at = stamp();
        task.digest = canonical(encode(task));
        sql::Statement update(db,
            "UPDATE conversation_tasks SET current_turn_id=?,current_run_id=?,"
            "revision=?,requirement_revision=?,updated_at=?,digest=? "
            "WHERE tenant=? AND conversation=? AND task_id=? AND revision=?");
        sql::bind_text(update.get(), 1, task.current_turn_id);
        sql::bind_text(update.get(), 2, task.current_run_id);
        sql::bind_uint64(update.get(), 3, task.revision);
        sql::bind_uint64(update.get(), 4, task.requirement_revision);
        sql::bind_text(update.get(), 5, task.updated_at);
        sql::bind_text(update.get(), 6, task.digest);
        sql::bind_text(update.get(), 7, task.identity.tenant_id);
        sql::bind_text(update.get(), 8, task.identity.conversation_id);
        sql::bind_text(update.get(), 9, task.task_id);
        sql::bind_uint64(update.get(), 10, expected);
        if(sql::step(update.get()) != SQLITE_DONE || sql::changes(db) != 1)
            throw std::runtime_error("task_revision_conflict");
        sql::Statement active_update(db,
            "UPDATE conversation_active_tasks SET task_revision=?,updated_at=? "
            "WHERE tenant=? AND conversation=? AND task_id=?");
        sql::bind_uint64(active_update.get(), 1, task.revision);
        sql::bind_text(active_update.get(), 2, task.updated_at);
        sql::bind_text(active_update.get(), 3, task.identity.tenant_id);
        sql::bind_text(active_update.get(), 4, task.identity.conversation_id);
        sql::bind_text(active_update.get(), 5, task.task_id);
        if(sql::step(active_update.get()) != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(db));
        append_event(db, task, "requirement_revised",
            {{"turn_id", input.turn_id}, {"run_id", run_id},
             {"intent", name(input.intent)}, {"requirement_revision", requirement.revision}});
        transaction.commit();
        return {true, task.revision, {}};
    } catch(const std::exception& error) {
        return {false, 0, error.what()};
    }
}

TaskMutationResult SQLiteTaskRegistry::transition(
    const ConversationIdentity& identity, std::string_view task_id,
    std::uint64_t expected, TaskLifecycleState state,
    std::string_view closure_state) {
    std::lock_guard lock(mutex_);
    auto* db = sql::database(db_);
    try {
        sql::Transaction transaction(db);
        sql::Statement old(db,
            "SELECT task_id,root_turn_id,current_turn_id,current_run_id,parent_task_id,"
            "state,closure_state,revision,requirement_revision,plan_revision,"
            "created_at,updated_at,digest FROM conversation_tasks "
            "WHERE tenant=? AND conversation=? AND task_id=?");
        bind_identity(old.get(), identity);
        sql::bind_text(old.get(), 3, task_id);
        if(sql::step(old.get()) != SQLITE_ROW) return {false, 0, "task_not_found"};
        auto task = task_from(old.get(), identity);
        if(task.revision != expected) return {false, task.revision, "task_revision_conflict"};
        task.state = state;
        task.closure_state = std::string(closure_state);
        ++task.revision;
        task.updated_at = stamp();
        task.digest = canonical(encode(task));
        sql::Statement update(db,
            "UPDATE conversation_tasks SET state=?,closure_state=?,revision=?,"
            "updated_at=?,digest=? WHERE tenant=? AND conversation=? AND task_id=? AND revision=?");
        sql::bind_text(update.get(), 1, name(task.state));
        sql::bind_text(update.get(), 2, task.closure_state);
        sql::bind_uint64(update.get(), 3, task.revision);
        sql::bind_text(update.get(), 4, task.updated_at);
        sql::bind_text(update.get(), 5, task.digest);
        sql::bind_text(update.get(), 6, identity.tenant_id);
        sql::bind_text(update.get(), 7, identity.conversation_id);
        sql::bind_text(update.get(), 8, task.task_id);
        sql::bind_uint64(update.get(), 9, expected);
        if(sql::step(update.get()) != SQLITE_DONE || sql::changes(db) != 1)
            throw std::runtime_error("task_revision_conflict");
        if(state == TaskLifecycleState::Closed || state == TaskLifecycleState::Failed ||
           state == TaskLifecycleState::Cancelled) {
            sql::Statement clear(db,
                "DELETE FROM conversation_active_tasks WHERE tenant=? AND conversation=? "
                "AND task_id=?");
            bind_identity(clear.get(), identity);
            sql::bind_text(clear.get(), 3, task.task_id);
            if(sql::step(clear.get()) != SQLITE_DONE)
                throw std::runtime_error(sqlite3_errmsg(db));
        } else {
            sql::Statement update_active(db,
                "UPDATE conversation_active_tasks SET task_revision=?,updated_at=? "
                "WHERE tenant=? AND conversation=? AND task_id=?");
            sql::bind_uint64(update_active.get(), 1, task.revision);
            sql::bind_text(update_active.get(), 2, task.updated_at);
            sql::bind_text(update_active.get(), 3, identity.tenant_id);
            sql::bind_text(update_active.get(), 4, identity.conversation_id);
            sql::bind_text(update_active.get(), 5, task.task_id);
            if(sql::step(update_active.get()) != SQLITE_DONE)
                throw std::runtime_error(sqlite3_errmsg(db));
        }
        append_event(db, task, "task_transitioned",
            {{"state", name(state)}, {"closure_state", closure_state}});
        transaction.commit();
        return {true, task.revision, {}};
    } catch(const std::exception& error) {
        return {false, 0, error.what()};
    }
}

std::optional<TurnTaskLink> SQLiteTaskRegistry::link_for_turn(
    const ConversationIdentity& identity, std::string_view turn_id) {
    std::lock_guard lock(mutex_);
    auto* db = sql::database(db_);
    sql::Statement query(db,
        "SELECT task_id,run_id,requirement_revision,intent,created_at "
        "FROM task_turn_links WHERE tenant=? AND conversation=? AND turn_id=?");
    bind_identity(query.get(), identity);
    sql::bind_text(query.get(), 3, turn_id);
    if(sql::step(query.get()) != SQLITE_ROW) return std::nullopt;
    TurnTaskLink value;
    value.identity = identity;
    value.turn_id = std::string(turn_id);
    value.task_id = sql::column_text(query.get(), 0);
    value.run_id = sql::column_text(query.get(), 1);
    value.requirement_revision = sql::column_uint64(query.get(), 2);
    value.intent = *task_input_intent(sql::column_text(query.get(), 3));
    value.created_at = sql::column_text(query.get(), 4);
    return value;
}

std::vector<TaskRequirementRevision> SQLiteTaskRegistry::requirements(
    const ConversationIdentity& identity, std::string_view task_id) {
    std::lock_guard lock(mutex_);
    auto* db = sql::database(db_);
    sql::Statement query(db,
        "SELECT revision,intent,turn_id,content,previous_digest,digest,created_at "
        "FROM task_requirement_revisions WHERE tenant=? AND conversation=? AND task_id=? "
        "ORDER BY revision");
    bind_identity(query.get(), identity);
    sql::bind_text(query.get(), 3, task_id);
    std::vector<TaskRequirementRevision> result;
    std::string previous;
    while(sql::step(query.get()) == SQLITE_ROW) {
        TaskRequirementRevision value;
        value.identity = identity;
        value.task_id = std::string(task_id);
        value.revision = sql::column_uint64(query.get(), 0);
        value.intent = *task_input_intent(sql::column_text(query.get(), 1));
        value.turn_id = sql::column_text(query.get(), 2);
        value.content = sql::column_text(query.get(), 3);
        value.previous_digest = sql::column_text(query.get(), 4);
        value.digest = sql::column_text(query.get(), 5);
        value.created_at = sql::column_text(query.get(), 6);
        if(value.previous_digest != previous || canonical(encode(value)) != value.digest)
            throw std::runtime_error("task requirement digest chain mismatch");
        previous = value.digest;
        result.push_back(std::move(value));
    }
    return result;
}

std::vector<TaskRunLink> SQLiteTaskRegistry::runs(
    const ConversationIdentity& identity, std::string_view task_id) {
    std::lock_guard lock(mutex_);
    auto* db = sql::database(db_);
    sql::Statement query(db,"SELECT run_id,requirement_revision,plan_revision,state,"
        "created_at,updated_at FROM task_run_links WHERE tenant=? AND conversation=? "
        "AND task_id=? ORDER BY created_at,run_id");
    bind_identity(query.get(),identity); sql::bind_text(query.get(),3,task_id);
    std::vector<TaskRunLink> result;
    while(sql::step(query.get()) == SQLITE_ROW) result.push_back({identity,std::string(task_id),
        sql::column_text(query.get(),0),sql::column_uint64(query.get(),1),
        sql::column_uint64(query.get(),2),sql::column_text(query.get(),3),
        sql::column_text(query.get(),4),sql::column_text(query.get(),5)});
    return result;
}

}  // namespace agent_framework::conversation
