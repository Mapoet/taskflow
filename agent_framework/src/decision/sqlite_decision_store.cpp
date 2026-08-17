#include "agent/decision/decision_store.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>

#include <sqlite3.h>

#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::decision {
namespace {
namespace sql=agent_framework::internal::sqlite;
using nlohmann::json;

std::string encode_options(const std::vector<DecisionOption>& options) {
    auto value=json::array();
    for(const auto& option:options)value.push_back({{"option_id",option.option_id},
        {"label",option.label},{"description",option.description},
        {"semantic_patch",option.semantic_patch}});
    return value.dump();
}

std::vector<DecisionOption> decode_options(std::string_view document) {
    std::vector<DecisionOption> result;
    for(const auto& item:json::parse(document))result.push_back({
        item.at("option_id").get<std::string>(),item.at("label").get<std::string>(),
        item.value("description",std::string{}),
        item.value("semantic_patch",json::object())});
    return result;
}

bool allowed_patch(DecisionKind kind,const json& patch) {
    if(!patch.is_object()||patch.dump().size()>2048)return false;
    const std::map<DecisionKind,std::set<std::string>> allowed{
        {DecisionKind::TaskSemantics,{"intent","work_shape","effect_class",
            "assurance_tier","compatibility_profile"}},
        {DecisionKind::Planning,{"planning_depth","scope","action"}},
        {DecisionKind::MemoryConflict,{"selected_memory_ids","resolution"}},
        {DecisionKind::RunRouting,{"route","target_session_id"}},
        {DecisionKind::Recovery,{"action","operation_id"}}};
    for(auto it=patch.begin();it!=patch.end();++it)
        if(!allowed.at(kind).contains(it.key()))return false;
    if(kind!=DecisionKind::TaskSemantics)return true;
    const auto valid_string=[&](const char* key,const std::set<std::string>& values) {
        const auto found=patch.find(key);return found==patch.end()||
            (found->is_string()&&values.contains(found->get<std::string>()));};
    return valid_string("intent",{"new_task","continue","add_requirement","narrow_scope",
        "replan","pause","cancel","status_query","profile_confirmation"})&&
        valid_string("work_shape",{"single_turn","bounded_task","long_running_task",
            "continuous_task"})&&
        valid_string("effect_class",{"none","read_only","workspace_write","external",
            "destructive"})&&
        valid_string("assurance_tier",{"basic","functional","professional",
            "production_certification"})&&
        valid_string("compatibility_profile",{"conversation","read_only_analysis",
            "artifact_delivery","code_change","external_action","professional"});
}

bool valid_options(DecisionKind kind,const std::vector<DecisionOption>& options) {
    if(options.size()<2||options.size()>8)return false;
    std::set<std::string> ids;
    for(const auto& option:options)
        if(option.option_id.empty()||option.option_id.size()>64||option.label.empty()||
           option.label.size()>128||option.description.size()>512||
           !allowed_patch(kind,option.semantic_patch)||!ids.insert(option.option_id).second)
            return false;
    return true;
}

DecisionRequest decode(sqlite3_stmt* row) {
    DecisionRequest value;
    value.subject.tenant_id=sql::column_text(row,0);
    value.decision_id=sql::column_text(row,1);
    value.subject.organization_id=sql::column_text(row,2);
    value.subject.principal_id=sql::column_text(row,3);
    value.subject.project_id=sql::column_text(row,4);
    value.subject.workspace_id=sql::column_text(row,5);
    value.subject.session_id=sql::column_text(row,6);
    value.subject.conversation_id=sql::column_text(row,7);
    value.subject.task_id=sql::column_text(row,8);
    value.subject.run_id=sql::column_text(row,9);
    value.subject.turn_id=sql::column_text(row,10);
    value.subject.agent_id=sql::column_text(row,11);
    value.subject.authorization_revision=sql::column_uint64(row,12);
    value.subject.authenticated=true;
    value.kind=*decision_kind(sql::column_text(row,13));
    value.resume_payload=json::parse(sql::column_text(row,14));
    value.question=sql::column_text(row,15);
    value.options=decode_options(sql::column_text(row,16));
    value.recommended_option_id=sql::column_text(row,17);
    value.selected_option_id=sql::column_text(row,18);
    value.revision=sql::column_uint64(row,19);
    value.expires_at_ms=sql::column_uint64(row,20);
    value.state=*decision_state(sql::column_text(row,21));
    value.origin_digest=sql::column_text(row,22);
    value.created_at=sql::column_text(row,23);
    value.updated_at=sql::column_text(row,24);
    return value;
}

constexpr const char* columns="tenant,decision_id,organization_id,principal_id,project_id,"
    "workspace_id,session_id,conversation_id,task_id,run_id,turn_id,agent_id,"
    "authorization_revision,kind,resume_payload_json,question,"
    "options_json,recommended_option_id,selected_option_id,revision,expires_at_ms,state,"
    "origin_digest,created_at,updated_at";
}

std::string_view name(DecisionKind value) {
    static constexpr std::array<std::string_view,5> names{
        "task_semantics","planning","memory_conflict","run_routing","recovery"};
    return names.at(static_cast<std::size_t>(value));
}
std::optional<DecisionKind> decision_kind(std::string_view value) {
    for(std::size_t i=0;i<5;++i)if(name(static_cast<DecisionKind>(i))==value)
        return static_cast<DecisionKind>(i);return {};
}
std::string_view name(DecisionState value) {
    static constexpr std::array<std::string_view,4> names{
        "pending","answered","expired","cancelled"};
    return names.at(static_cast<std::size_t>(value));
}
std::optional<DecisionState> decision_state(std::string_view value) {
    for(std::size_t i=0;i<4;++i)if(name(static_cast<DecisionState>(i))==value)
        return static_cast<DecisionState>(i);return {};
}

SQLiteDecisionStore::SQLiteDecisionStore(std::string path):path_(std::move(path)) {
    if(path_.empty())throw std::invalid_argument("decision store path required");
    std::error_code error;const std::filesystem::path file(path_);
    if(file.has_parent_path())std::filesystem::create_directories(file.parent_path(),error);
    sqlite3* opened=nullptr;
    if(error||sqlite3_open_v2(path_.c_str(),&opened,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|
       SQLITE_OPEN_FULLMUTEX,nullptr)!=SQLITE_OK) {
        const std::string message=opened?sqlite3_errmsg(opened):error.message();
        if(opened)sqlite3_close(opened);throw std::runtime_error(message);
    }
    db_=opened;sqlite3_busy_timeout(opened,3000);sql::exec(opened,"PRAGMA journal_mode=WAL");
    sql::exec(opened,"PRAGMA synchronous=FULL");migrate();
}
SQLiteDecisionStore::~SQLiteDecisionStore(){if(db_)sqlite3_close(sql::database(db_));}

void SQLiteDecisionStore::migrate() {
    auto* db=sql::database(db_);
    sql::exec(db,
        "CREATE TABLE IF NOT EXISTS durable_decisions("
        "tenant TEXT NOT NULL,decision_id TEXT NOT NULL,organization_id TEXT NOT NULL,"
        "principal_id TEXT NOT NULL,project_id TEXT NOT NULL,workspace_id TEXT NOT NULL,"
        "session_id TEXT NOT NULL,conversation_id TEXT NOT NULL,task_id TEXT NOT NULL,"
        "run_id TEXT NOT NULL,turn_id TEXT NOT NULL,agent_id TEXT NOT NULL,"
        "authorization_revision INTEGER NOT NULL,kind TEXT NOT NULL,"
        "resume_payload_json TEXT NOT NULL,question TEXT NOT NULL,"
        "options_json TEXT NOT NULL,recommended_option_id TEXT NOT NULL,"
        "selected_option_id TEXT NOT NULL DEFAULT '',revision INTEGER NOT NULL,"
        "expires_at_ms INTEGER NOT NULL,state TEXT NOT NULL,origin_digest TEXT NOT NULL,"
        "created_at TEXT NOT NULL,updated_at TEXT NOT NULL,PRIMARY KEY(tenant,decision_id))");
    const auto add=[&](std::string_view name,std::string_view definition) {
        if(!sql::table_has_column(db,"durable_decisions",name)) {
            const auto statement=std::string("ALTER TABLE durable_decisions ADD COLUMN ")+std::string(definition);
            sql::exec(db,statement.c_str());
        }};
    add("agent_id","agent_id TEXT NOT NULL DEFAULT ''");
    add("authorization_revision","authorization_revision INTEGER NOT NULL DEFAULT 0");
    add("resume_payload_json","resume_payload_json TEXT NOT NULL DEFAULT '{}'");
    sql::exec(db,"CREATE INDEX IF NOT EXISTS durable_decisions_pending_idx ON "
        "durable_decisions(tenant,session_id,conversation_id,state,created_at)");
}

DecisionMutationResult SQLiteDecisionStore::create(DecisionRequest value) {
    if(value.subject.tenant_id.empty()||value.subject.session_id.empty()||
       value.subject.conversation_id.empty()||value.decision_id.empty()||
       value.question.empty()||value.question.size()>1024||!value.resume_payload.is_object()||
       !valid_options(value.kind,value.options)||
       value.expires_at_ms==0||value.origin_digest.empty())
        return {false,0,value.state,"decision_contract_invalid"};
    if(!value.recommended_option_id.empty()&&std::none_of(value.options.begin(),value.options.end(),
       [&](const auto& option){return option.option_id==value.recommended_option_id;}))
        return {false,0,value.state,"decision_recommendation_invalid"};
    std::lock_guard lock(mutex_);auto* db=sql::database(db_);
    try {
        sql::Transaction transaction(db);sql::Statement existing(db,
            "SELECT origin_digest,revision,state FROM durable_decisions WHERE tenant=? AND decision_id=?");
        sql::bind_text(existing.get(),1,value.subject.tenant_id);sql::bind_text(existing.get(),2,value.decision_id);
        if(sql::step(existing.get())==SQLITE_ROW) {
            const bool same=sql::column_text(existing.get(),0)==value.origin_digest;
            return {same,sql::column_uint64(existing.get(),1),
                *decision_state(sql::column_text(existing.get(),2)),
                same?"":"decision_idempotency_conflict"};
        }
        value.revision=1;value.state=DecisionState::Pending;
        const auto insert_sql=std::string("INSERT INTO durable_decisions(")+columns+
            ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
        sql::Statement insert(db,insert_sql.c_str());
        const std::array<std::string,12> identity_values={value.subject.tenant_id,value.decision_id,
            value.subject.organization_id,value.subject.principal_id,value.subject.project_id,
            value.subject.workspace_id,value.subject.session_id,value.subject.conversation_id,
            value.subject.task_id,value.subject.run_id,value.subject.turn_id,value.subject.agent_id};
        int index=1;for(const auto& item:identity_values)sql::bind_text(insert.get(),index++,item);
        sql::bind_uint64(insert.get(),index++,value.subject.authorization_revision);
        const std::array<std::string,6> decision_values={std::string(name(value.kind)),
            value.resume_payload.dump(),value.question,
            encode_options(value.options),value.recommended_option_id,value.selected_option_id};
        for(const auto& item:decision_values)sql::bind_text(insert.get(),index++,item);
        sql::bind_uint64(insert.get(),index++,value.revision);sql::bind_uint64(insert.get(),index++,value.expires_at_ms);
        sql::bind_text(insert.get(),index++,name(value.state));sql::bind_text(insert.get(),index++,value.origin_digest);
        sql::bind_text(insert.get(),index++,value.created_at);sql::bind_text(insert.get(),index++,value.updated_at);
        if(sql::step(insert.get())!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(db));
        transaction.commit();return {true,1,value.state,{}};
    } catch(const std::exception& error){return {false,0,value.state,error.what()};}
}

std::optional<DecisionRequest> SQLiteDecisionStore::load(std::string_view tenant,std::string_view id) {
    std::lock_guard lock(mutex_);const auto query=std::string("SELECT ")+columns+
        " FROM durable_decisions WHERE tenant=? AND decision_id=?";
    sql::Statement statement(sql::database(db_),query.c_str());sql::bind_text(statement.get(),1,tenant);
    sql::bind_text(statement.get(),2,id);return sql::step(statement.get())==SQLITE_ROW
        ?std::optional<DecisionRequest>(decode(statement.get())):std::nullopt;
}

std::optional<DecisionRequest> SQLiteDecisionStore::pending(std::string_view tenant,
    std::string_view session,std::string_view conversation) {
    std::lock_guard lock(mutex_);const auto query=std::string("SELECT ")+columns+
        " FROM durable_decisions WHERE tenant=? AND session_id=? AND conversation_id=? "
        "AND state='pending' ORDER BY created_at DESC,decision_id DESC LIMIT 1";
    sql::Statement statement(sql::database(db_),query.c_str());sql::bind_text(statement.get(),1,tenant);
    sql::bind_text(statement.get(),2,session);sql::bind_text(statement.get(),3,conversation);
    return sql::step(statement.get())==SQLITE_ROW
        ?std::optional<DecisionRequest>(decode(statement.get())):std::nullopt;
}

std::optional<DecisionRequest> SQLiteDecisionStore::latest(std::string_view tenant,
    std::string_view session,std::string_view conversation) {
    std::lock_guard lock(mutex_);const auto query=std::string("SELECT ")+columns+
        " FROM durable_decisions WHERE tenant=? AND session_id=? AND conversation_id=? "
        "ORDER BY created_at DESC,decision_id DESC LIMIT 1";
    sql::Statement statement(sql::database(db_),query.c_str());sql::bind_text(statement.get(),1,tenant);
    sql::bind_text(statement.get(),2,session);sql::bind_text(statement.get(),3,conversation);
    return sql::step(statement.get())==SQLITE_ROW
        ?std::optional<DecisionRequest>(decode(statement.get())):std::nullopt;
}

DecisionMutationResult SQLiteDecisionStore::answer(std::string_view tenant,std::string_view id,
    std::uint64_t expected,std::string_view option_id,std::uint64_t now_ms) {
    std::lock_guard lock(mutex_);auto* db=sql::database(db_);
    try {
        sql::Transaction transaction(db);const auto query=std::string("SELECT ")+columns+
            " FROM durable_decisions WHERE tenant=? AND decision_id=?";
        sql::Statement find(db,query.c_str());sql::bind_text(find.get(),1,tenant);sql::bind_text(find.get(),2,id);
        if(sql::step(find.get())!=SQLITE_ROW)return {false,0,DecisionState::Pending,"decision_not_found"};
        const auto current=decode(find.get());
        if(current.revision!=expected)return {false,current.revision,current.state,"decision_revision_conflict"};
        if(current.state!=DecisionState::Pending)return {false,current.revision,current.state,"decision_not_pending"};
        if(now_ms>=current.expires_at_ms)return {false,current.revision,current.state,"decision_expired"};
        if(std::none_of(current.options.begin(),current.options.end(),
           [&](const auto& option){return option.option_id==option_id;}))
            return {false,current.revision,current.state,"decision_option_invalid"};
        sql::Statement update(db,"UPDATE durable_decisions SET selected_option_id=?,revision=revision+1,"
            "state='answered',updated_at=? WHERE tenant=? AND decision_id=? AND revision=? AND state='pending'");
        sql::bind_text(update.get(),1,option_id);sql::bind_text(update.get(),2,std::to_string(now_ms));
        sql::bind_text(update.get(),3,tenant);sql::bind_text(update.get(),4,id);sql::bind_uint64(update.get(),5,expected);
        if(sql::step(update.get())!=SQLITE_DONE||sql::changes(db)!=1)
            return {false,current.revision,current.state,"decision_revision_conflict"};
        transaction.commit();return {true,expected+1,DecisionState::Answered,{}};
    } catch(const std::exception& error){return {false,expected,DecisionState::Pending,error.what()};}
}

DecisionMutationResult SQLiteDecisionStore::cancel(std::string_view tenant,std::string_view id,
    std::uint64_t expected) {
    std::lock_guard lock(mutex_);auto* db=sql::database(db_);sql::Statement update(db,
        "UPDATE durable_decisions SET revision=revision+1,state='cancelled' WHERE tenant=? AND "
        "decision_id=? AND revision=? AND state='pending'");
    sql::bind_text(update.get(),1,tenant);sql::bind_text(update.get(),2,id);sql::bind_uint64(update.get(),3,expected);
    if(sql::step(update.get())!=SQLITE_DONE||sql::changes(db)!=1)
        return {false,expected,DecisionState::Pending,"decision_revision_conflict"};
    return {true,expected+1,DecisionState::Cancelled,{}};
}

DecisionMutationResult SQLiteDecisionStore::expire(std::string_view tenant,std::string_view id,
    std::uint64_t expected,std::uint64_t now_ms) {
    std::lock_guard lock(mutex_);auto* db=sql::database(db_);sql::Statement update(db,
        "UPDATE durable_decisions SET revision=revision+1,state='expired',updated_at=? WHERE tenant=? "
        "AND decision_id=? AND revision=? AND state='pending' AND expires_at_ms<=?");
    sql::bind_text(update.get(),1,std::to_string(now_ms));sql::bind_text(update.get(),2,tenant);
    sql::bind_text(update.get(),3,id);sql::bind_uint64(update.get(),4,expected);sql::bind_uint64(update.get(),5,now_ms);
    if(sql::step(update.get())!=SQLITE_DONE||sql::changes(db)!=1)
        return {false,expected,DecisionState::Pending,"decision_not_expired_or_revision_conflict"};
    return {true,expected+1,DecisionState::Expired,{}};
}

}  // namespace agent_framework::decision
