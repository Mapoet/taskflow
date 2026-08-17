#include "agent/conversation/task_profile_clarification.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::conversation {
namespace {
namespace sql = agent_framework::internal::sqlite;

std::string trimmed_lower(std::string_view input) {
    std::string value(input);
    while(!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while(!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

void bind_identity(sqlite3_stmt* statement, const ConversationIdentity& identity) {
    sql::bind_text(statement, 1, identity.tenant_id);
    sql::bind_text(statement, 2, identity.conversation_id);
}

std::string encode_tokens(const std::vector<std::string>& tokens) {
    return nlohmann::json(tokens).dump();
}

std::vector<std::string> decode_tokens(std::string_view value) {
    try { return nlohmann::json::parse(value).get<std::vector<std::string>>(); }
    catch(...) { throw std::runtime_error("stored clarification tokens invalid"); }
}

std::string encode_options(const std::vector<TaskClarificationOption>& options) {
    auto values=nlohmann::json::array();
    for(const auto& option:options)values.push_back({{"id",option.id},{"label",option.label},
        {"description",option.description},{"profile",name(option.profile)},
        {"semantic_patch",option.semantic_patch}});
    return values.dump();
}

std::vector<TaskClarificationOption> decode_options(std::string_view value) {
    std::vector<TaskClarificationOption> result;
    if(value.empty())return result;
    try {
        for(const auto& item:nlohmann::json::parse(value)) {
            const auto profile=task_execution_profile(item.at("profile").get<std::string>());
            if(!profile)throw std::runtime_error("stored clarification option profile invalid");
            result.push_back({item.at("id").get<std::string>(),item.at("label").get<std::string>(),
                item.value("description",std::string{}),*profile,
                item.value("semantic_patch",nlohmann::json::object())});
        }
        return result;
    } catch(...) { throw std::runtime_error("stored clarification options invalid"); }
}

std::optional<ProfileClarificationState> state_from(std::string_view value) {
    for(std::size_t i = 0; i < 5; ++i) {
        const auto state = static_cast<ProfileClarificationState>(i);
        if(name(state) == value) return state;
    }
    return std::nullopt;
}

TaskProfileClarification decode(sqlite3_stmt* row,
                                const ConversationIdentity& identity) {
    TaskProfileClarification value;
    value.identity = identity;
    value.clarification_id = sql::column_text(row, 0);
    value.task_id = sql::column_text(row, 1);
    value.decision_id = sql::column_text(row, 2);
    value.turn_id = sql::column_text(row, 3);
    value.run_id = sql::column_text(row, 4);
    value.task_intent = *task_input_intent(sql::column_text(row, 5));
    value.recommended_profile = *task_execution_profile(sql::column_text(row, 6));
    const auto selected = sql::column_text(row, 7);
    if(!selected.empty()) value.selected_profile = *task_execution_profile(selected);
    value.allowed_tokens = decode_tokens(sql::column_text(row, 8));
    value.attempt_count = static_cast<std::uint32_t>(sql::column_uint64(row, 9));
    value.max_attempts = static_cast<std::uint32_t>(sql::column_uint64(row, 10));
    value.revision = sql::column_uint64(row, 11);
    value.expires_at_ms = sql::column_uint64(row, 12);
    value.state = *state_from(sql::column_text(row, 13));
    value.created_at = sql::column_text(row, 14);
    value.updated_at = sql::column_text(row, 15);
    value.question = sql::column_text(row, 16);
    value.options = decode_options(sql::column_text(row, 17));
    return value;
}

constexpr const char* select_columns =
    "clarification_id,task_id,decision_id,turn_id,run_id,task_intent,"
    "recommended_profile,selected_profile,allowed_tokens_json,attempt_count,"
    "max_attempts,revision,expires_at_ms,state,"
    "created_at,updated_at,question,options_json";
}  // namespace

std::string_view name(ProfileClarificationState value) {
    static constexpr std::array<std::string_view, 5> values{
        "pending", "confirmed", "exhausted", "expired", "cancelled"};
    const auto index = static_cast<std::size_t>(value);
    return index < values.size() ? values[index] : "unknown";
}

ProfileConfirmation parse_profile_confirmation(std::string_view input) {
    const auto token = trimmed_lower(input);
    const auto profile = task_execution_profile(token);
    if(!profile) return {{}, "profile_confirmation_requires_one_canonical_token"};
    return {profile, {}};
}

SQLiteTaskProfileClarificationStore::SQLiteTaskProfileClarificationStore(
    std::string database_path) : path_(std::move(database_path)) {
    if(path_.empty()) throw std::invalid_argument("clarification store path required");
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
    migrate();
}

SQLiteTaskProfileClarificationStore::~SQLiteTaskProfileClarificationStore() {
    if(db_) sqlite3_close(sql::database(db_));
}

void SQLiteTaskProfileClarificationStore::migrate() {
    auto* db=sql::database(db_);
    sql::exec(db,
        "CREATE TABLE IF NOT EXISTS task_profile_clarifications("
        "tenant TEXT NOT NULL,conversation TEXT NOT NULL,clarification_id TEXT NOT NULL,"
        "task_id TEXT NOT NULL,decision_id TEXT NOT NULL,turn_id TEXT NOT NULL,"
        "run_id TEXT NOT NULL,task_intent TEXT NOT NULL,recommended_profile TEXT NOT NULL,"
        "selected_profile TEXT NOT NULL DEFAULT '',allowed_tokens_json TEXT NOT NULL,"
        "attempt_count INTEGER NOT NULL,max_attempts INTEGER NOT NULL,revision INTEGER NOT NULL,"
        "expires_at_ms INTEGER NOT NULL,state TEXT NOT NULL,created_at TEXT NOT NULL,"
        "updated_at TEXT NOT NULL,PRIMARY KEY(tenant,conversation,clarification_id))");
    auto add_column=[db](std::string_view column,std::string_view definition){
        if(!sql::table_has_column(db,"task_profile_clarifications",column)){
            const auto statement=std::string("ALTER TABLE task_profile_clarifications ADD COLUMN ")+std::string(definition);sql::exec(db,statement.c_str());}
    };
    add_column("turn_id","turn_id TEXT NOT NULL DEFAULT ''");
    add_column("run_id","run_id TEXT NOT NULL DEFAULT ''");
    add_column("task_intent","task_intent TEXT NOT NULL DEFAULT 'initial_request'");
    add_column("question","question TEXT NOT NULL DEFAULT ''");
    add_column("options_json","options_json TEXT NOT NULL DEFAULT '[]'");
    sql::exec(db,"UPDATE task_profile_clarifications SET state='cancelled' WHERE state='pending' AND (turn_id='' OR run_id='')");
    sql::exec(db,"UPDATE task_profile_clarifications SET state='cancelled' WHERE state='pending' AND (question='' OR options_json='[]')");
    sql::exec(db,
        "CREATE INDEX IF NOT EXISTS task_profile_clarification_pending_idx ON "
        "task_profile_clarifications(tenant,conversation,state,created_at)");
}

ClarificationMutationResult SQLiteTaskProfileClarificationStore::create(
    TaskProfileClarification value) {
    if(value.identity.tenant_id.empty() || value.identity.conversation_id.empty() ||
       value.clarification_id.empty() || value.decision_id.empty() ||
       value.turn_id.empty() || value.run_id.empty() || value.task_id.empty() ||
       value.question.empty() || value.options.size()<2 || value.allowed_tokens.empty() ||
       value.max_attempts == 0 ||
       value.expires_at_ms == 0)
        return {false, 0, value.state, "clarification_contract_invalid"};
    std::lock_guard lock(mutex_);
    auto* db = sql::database(db_);
    try {
        sql::Transaction transaction(db);
        sql::Statement existing(db,
            "SELECT decision_id,revision,state FROM task_profile_clarifications "
            "WHERE tenant=? AND conversation=? AND clarification_id=?");
        bind_identity(existing.get(), value.identity);
        sql::bind_text(existing.get(), 3, value.clarification_id);
        if(sql::step(existing.get()) == SQLITE_ROW) {
            const bool same = sql::column_text(existing.get(), 0) == value.decision_id;
            return {same, sql::column_uint64(existing.get(), 1),
                    *state_from(sql::column_text(existing.get(), 2)),
                    same ? "" : "clarification_idempotency_conflict"};
        }
        value.revision = 1;
        value.attempt_count = 0;
        value.state = ProfileClarificationState::Pending;
        sql::Statement insert(db,
            "INSERT INTO task_profile_clarifications(tenant,conversation,clarification_id,task_id,"
            "decision_id,turn_id,run_id,task_intent,recommended_profile,selected_profile,"
            "allowed_tokens_json,attempt_count,max_attempts,revision,expires_at_ms,state,created_at,"
            "updated_at,question,options_json) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        bind_identity(insert.get(), value.identity);
        sql::bind_text(insert.get(), 3, value.clarification_id);
        sql::bind_text(insert.get(), 4, value.task_id);
        sql::bind_text(insert.get(), 5, value.decision_id);
        sql::bind_text(insert.get(), 6, value.turn_id);
        sql::bind_text(insert.get(), 7, value.run_id);
        sql::bind_text(insert.get(), 8, name(value.task_intent));
        sql::bind_text(insert.get(), 9, name(value.recommended_profile));
        sql::bind_text(insert.get(), 10, "");
        sql::bind_text(insert.get(), 11, encode_tokens(value.allowed_tokens));
        sql::bind_uint64(insert.get(), 12, value.attempt_count);
        sql::bind_uint64(insert.get(), 13, value.max_attempts);
        sql::bind_uint64(insert.get(), 14, value.revision);
        sql::bind_uint64(insert.get(), 15, value.expires_at_ms);
        sql::bind_text(insert.get(), 16, name(value.state));
        sql::bind_text(insert.get(), 17, value.created_at);
        sql::bind_text(insert.get(), 18, value.updated_at);
        sql::bind_text(insert.get(), 19, value.question);
        sql::bind_text(insert.get(), 20, encode_options(value.options));
        if(sql::step(insert.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
        transaction.commit();
        return {true, 1, value.state, {}};
    } catch(const std::exception& e) { return {false, 0, value.state, e.what()}; }
}

std::optional<TaskProfileClarification> SQLiteTaskProfileClarificationStore::load(
    const ConversationIdentity& identity, std::string_view id) {
    std::lock_guard lock(mutex_);
    const auto query = std::string("SELECT ") + select_columns +
        " FROM task_profile_clarifications WHERE tenant=? AND conversation=? AND clarification_id=?";
    sql::Statement statement(sql::database(db_), query.c_str());
    bind_identity(statement.get(), identity);
    sql::bind_text(statement.get(), 3, id);
    if(sql::step(statement.get()) != SQLITE_ROW) return std::nullopt;
    return decode(statement.get(), identity);
}

std::optional<TaskProfileClarification> SQLiteTaskProfileClarificationStore::pending(
    const ConversationIdentity& identity) {
    std::lock_guard lock(mutex_);
    const auto query = std::string("SELECT ") + select_columns +
        " FROM task_profile_clarifications WHERE tenant=? AND conversation=? "
        "AND state='pending' ORDER BY created_at DESC,clarification_id DESC LIMIT 1";
    sql::Statement statement(sql::database(db_), query.c_str());
    bind_identity(statement.get(), identity);
    if(sql::step(statement.get()) != SQLITE_ROW) return std::nullopt;
    return decode(statement.get(), identity);
}

std::optional<TaskProfileClarification> SQLiteTaskProfileClarificationStore::latest(
    const ConversationIdentity& identity) {
    std::lock_guard lock(mutex_);
    const auto query=std::string("SELECT ")+select_columns+
        " FROM task_profile_clarifications WHERE tenant=? AND conversation=? "
        "ORDER BY created_at DESC,clarification_id DESC LIMIT 1";
    sql::Statement statement(sql::database(db_),query.c_str());
    bind_identity(statement.get(),identity);
    if(sql::step(statement.get())!=SQLITE_ROW)return std::nullopt;
    return decode(statement.get(),identity);
}

ClarificationMutationResult SQLiteTaskProfileClarificationStore::answer(
    const ConversationIdentity& identity, std::string_view id,
    std::uint64_t expected, std::string_view input, std::uint64_t now_ms) {
    std::lock_guard lock(mutex_);
    auto* db = sql::database(db_);
    try {
        sql::Transaction transaction(db);
        const auto query = std::string("SELECT ") + select_columns +
            " FROM task_profile_clarifications WHERE tenant=? AND conversation=? AND clarification_id=?";
        sql::Statement find(db, query.c_str());
        bind_identity(find.get(), identity);
        sql::bind_text(find.get(), 3, id);
        if(sql::step(find.get()) != SQLITE_ROW)
            return {false, 0, ProfileClarificationState::Pending, "clarification_not_found"};
        auto current = decode(find.get(), identity);
        if(current.revision != expected)
            return {false, current.revision, current.state, "clarification_revision_conflict"};
        if(current.state != ProfileClarificationState::Pending)
            return {false, current.revision, current.state, "clarification_not_pending"};

        const auto selected_option=std::find_if(current.options.begin(),current.options.end(),
            [&](const auto& option){return option.id==input;});
        auto next_state = ProfileClarificationState::Pending;
        std::string selected;
        std::uint32_t attempts = current.attempt_count;
        std::string error;
        if(now_ms >= current.expires_at_ms) {
            next_state = ProfileClarificationState::Expired;
            error = "clarification_expired";
        } else if(selected_option==current.options.end()) {
            ++attempts;
            next_state = attempts >= current.max_attempts
                ? ProfileClarificationState::Exhausted
                : ProfileClarificationState::Pending;
            error = next_state == ProfileClarificationState::Exhausted
                ? "clarification_attempts_exhausted"
                : "profile_confirmation_requires_one_allowed_token";
        } else {
            next_state = ProfileClarificationState::Confirmed;
            selected = std::string(name(selected_option->profile));
        }
        sql::Statement update(db,
            "UPDATE task_profile_clarifications SET selected_profile=?,attempt_count=?,"
            "revision=revision+1,state=?,updated_at=? WHERE tenant=? AND conversation=? "
            "AND clarification_id=? AND revision=? AND state='pending'");
        sql::bind_text(update.get(), 1, selected);
        sql::bind_uint64(update.get(), 2, attempts);
        sql::bind_text(update.get(), 3, name(next_state));
        sql::bind_text(update.get(), 4, std::to_string(now_ms));
        sql::bind_text(update.get(), 5, identity.tenant_id);
        sql::bind_text(update.get(), 6, identity.conversation_id);
        sql::bind_text(update.get(), 7, id);
        sql::bind_uint64(update.get(), 8, expected);
        if(sql::step(update.get()) != SQLITE_DONE || sql::changes(db) != 1)
            return {false, current.revision, current.state, "clarification_revision_conflict"};
        transaction.commit();
        return {next_state == ProfileClarificationState::Confirmed, expected + 1,
                next_state, error};
    } catch(const std::exception& e) {
        return {false, expected, ProfileClarificationState::Pending, e.what()};
    }
}

ClarificationMutationResult SQLiteTaskProfileClarificationStore::cancel(
    const ConversationIdentity& identity, std::string_view id,
    std::uint64_t expected) {
    std::lock_guard lock(mutex_);
    auto* db = sql::database(db_);
    try {
        sql::Statement update(db,
            "UPDATE task_profile_clarifications SET revision=revision+1,state='cancelled' "
            "WHERE tenant=? AND conversation=? AND clarification_id=? AND revision=? AND state='pending'");
        bind_identity(update.get(), identity);
        sql::bind_text(update.get(), 3, id);
        sql::bind_uint64(update.get(), 4, expected);
        if(sql::step(update.get()) != SQLITE_DONE || sql::changes(db) != 1)
            return {false, expected, ProfileClarificationState::Pending,
                    "clarification_revision_conflict"};
        return {true, expected + 1, ProfileClarificationState::Cancelled, {}};
    } catch(const std::exception& e) {
        return {false, expected, ProfileClarificationState::Pending, e.what()};
    }
}

}  // namespace agent_framework::conversation
