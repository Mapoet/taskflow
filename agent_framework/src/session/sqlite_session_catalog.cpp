#include "agent/session/session_catalog.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::session {
namespace {
namespace sql = agent_framework::internal::sqlite;

std::string stamp() {
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
std::optional<ProductSessionState> state_from(std::string_view text) {
    for(std::size_t i=0;i<5;++i) { auto v=static_cast<ProductSessionState>(i); if(name(v)==text)return v; }
    return std::nullopt;
}
ProductSession decode(sqlite3_stmt* row) {
    ProductSession v;
    v.tenant_id=sql::column_text(row,0);v.organization_id=sql::column_text(row,1);
    v.project_id=sql::column_text(row,2);v.workspace_id=sql::column_text(row,3);
    v.session_id=sql::column_text(row,4);v.conversation_id=sql::column_text(row,5);
    v.owner_principal_id=sql::column_text(row,6);v.title=sql::column_text(row,7);
    v.folder=sql::column_text(row,8);
    try { v.tags=nlohmann::json::parse(sql::column_text(row,9)).get<std::vector<std::string>>(); }
    catch(...) { throw std::runtime_error("stored session tags invalid"); }
    v.pinned=sql::column_int(row,10)!=0;v.state=*state_from(sql::column_text(row,11));
    v.revision=sql::column_uint64(row,12);v.sequence=sql::column_uint64(row,13);
    v.created_at=sql::column_text(row,14);v.updated_at=sql::column_text(row,15);return v;
}
constexpr const char* columns="s.tenant,s.organization_id,s.project_id,s.workspace_id,"
    "s.session_id,s.conversation_id,s.owner_principal_id,s.title,s.folder,s.tags_json,"
    "s.pinned,s.state,s.revision,s.sequence,s.created_at,s.updated_at";
bool transition_allowed(ProductSessionState from,ProductSessionState to) {
    if(from==ProductSessionState::Active)
        return to==ProductSessionState::Archived||to==ProductSessionState::Trashed;
    if(from==ProductSessionState::Archived)
        return to==ProductSessionState::Active||to==ProductSessionState::Trashed;
    if(from==ProductSessionState::Trashed)
        return to==ProductSessionState::Active||to==ProductSessionState::PurgePending;
    return from==ProductSessionState::PurgePending&&to==ProductSessionState::Purged;
}
SessionMutationResult changed(sqlite3* db,std::uint64_t expected) {
    return sql::changes(db)==1?SessionMutationResult{true,expected+1,{}}
                              :SessionMutationResult{false,expected,"revision_conflict"};
}
}

std::string_view name(ProductSessionState v) {
    static constexpr std::array<std::string_view,5> n{"active","archived","trashed","purge_pending","purged"};
    auto i=static_cast<std::size_t>(v);return i<n.size()?n[i]:"unknown";
}
std::string_view name(SessionMemberRole v) {
    static constexpr std::array<std::string_view,4> n{"viewer","contributor","operator","owner"};
    auto i=static_cast<std::size_t>(v);return i<n.size()?n[i]:"unknown";
}

SQLiteSessionCatalog::SQLiteSessionCatalog(std::string path):path_(std::move(path)) {
    if(path_.empty())throw std::invalid_argument("session catalog path required");
    std::error_code error;const std::filesystem::path file(path_);
    if(file.has_parent_path())std::filesystem::create_directories(file.parent_path(),error);
    sqlite3* opened=nullptr;
    if(error||sqlite3_open_v2(path_.c_str(),&opened,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,nullptr)!=SQLITE_OK){
        std::string message=opened?sqlite3_errmsg(opened):error.message();if(opened)sqlite3_close(opened);throw std::runtime_error(message);}
    db_=opened;sqlite3_busy_timeout(opened,3000);sql::exec(opened,"PRAGMA journal_mode=WAL");
    sql::exec(opened,"PRAGMA synchronous=FULL");migrate();
}
SQLiteSessionCatalog::~SQLiteSessionCatalog(){if(db_)sqlite3_close(sql::database(db_));}
void SQLiteSessionCatalog::migrate(){auto* db=sql::database(db_);
    sql::exec(db,"CREATE TABLE IF NOT EXISTS product_session_sequence(id INTEGER PRIMARY KEY AUTOINCREMENT)");
    sql::exec(db,"CREATE TABLE IF NOT EXISTS product_sessions(tenant TEXT NOT NULL,organization_id TEXT NOT NULL,project_id TEXT NOT NULL,workspace_id TEXT NOT NULL,session_id TEXT NOT NULL,conversation_id TEXT NOT NULL,owner_principal_id TEXT NOT NULL,title TEXT NOT NULL,folder TEXT NOT NULL,tags_json TEXT NOT NULL,pinned INTEGER NOT NULL,state TEXT NOT NULL,revision INTEGER NOT NULL,sequence INTEGER NOT NULL UNIQUE,created_at TEXT NOT NULL,updated_at TEXT NOT NULL,PRIMARY KEY(tenant,session_id))");
    sql::exec(db,"CREATE INDEX IF NOT EXISTS product_sessions_list_idx ON product_sessions(tenant,state,sequence DESC)");
    sql::exec(db,"CREATE TABLE IF NOT EXISTS product_session_members(tenant TEXT NOT NULL,session_id TEXT NOT NULL,principal_id TEXT NOT NULL,role TEXT NOT NULL,revision INTEGER NOT NULL,PRIMARY KEY(tenant,session_id,principal_id))");
}
SessionMutationResult SQLiteSessionCatalog::create(ProductSession v){
    if(v.tenant_id.empty()||v.organization_id.empty()||v.project_id.empty()||v.workspace_id.empty()||v.session_id.empty()||v.conversation_id.empty()||v.owner_principal_id.empty()||v.title.empty())return {false,0,"identity_required"};
    std::lock_guard lock(mutex_);auto* db=sql::database(db_);try{sql::Transaction tx(db);
        sql::Statement find(db,"SELECT conversation_id,owner_principal_id,revision FROM product_sessions WHERE tenant=? AND session_id=?");sql::bind_text(find.get(),1,v.tenant_id);sql::bind_text(find.get(),2,v.session_id);
        if(sql::step(find.get())==SQLITE_ROW){bool same=sql::column_text(find.get(),0)==v.conversation_id&&sql::column_text(find.get(),1)==v.owner_principal_id;return {same,sql::column_uint64(find.get(),2),same?"":"idempotency_conflict"};}
        sql::exec(db,"INSERT INTO product_session_sequence DEFAULT VALUES");v.sequence=static_cast<std::uint64_t>(sql::last_insert_rowid(db));v.revision=1;if(v.created_at.empty())v.created_at=stamp();if(v.updated_at.empty())v.updated_at=v.created_at;
        sql::Statement insert(db,"INSERT INTO product_sessions VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        int i=1;for(const auto* s:{&v.tenant_id,&v.organization_id,&v.project_id,&v.workspace_id,&v.session_id,&v.conversation_id,&v.owner_principal_id,&v.title,&v.folder})sql::bind_text(insert.get(),i++,*s);
        sql::bind_text(insert.get(),i++,nlohmann::json(v.tags).dump());sql::bind_int(insert.get(),i++,v.pinned?1:0);sql::bind_text(insert.get(),i++,name(v.state));sql::bind_uint64(insert.get(),i++,v.revision);sql::bind_uint64(insert.get(),i++,v.sequence);sql::bind_text(insert.get(),i++,v.created_at);sql::bind_text(insert.get(),i++,v.updated_at);
        if(sql::step(insert.get())!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(db));
        sql::Statement member(db,"INSERT INTO product_session_members VALUES(?,?,?,?,1)");sql::bind_text(member.get(),1,v.tenant_id);sql::bind_text(member.get(),2,v.session_id);sql::bind_text(member.get(),3,v.owner_principal_id);sql::bind_text(member.get(),4,"owner");if(sql::step(member.get())!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(db));tx.commit();return {true,1,{}};
    }catch(const std::exception& e){return {false,0,e.what()};}}
std::optional<ProductSession> SQLiteSessionCatalog::get(std::string_view tenant,std::string_view session){std::lock_guard lock(mutex_);auto query=std::string("SELECT ")+columns+" FROM product_sessions s WHERE s.tenant=? AND s.session_id=?";sql::Statement q(sql::database(db_),query.c_str());sql::bind_text(q.get(),1,tenant);sql::bind_text(q.get(),2,session);if(sql::step(q.get())!=SQLITE_ROW)return {};return decode(q.get());}
SessionListPage SQLiteSessionCatalog::list(const SessionListQuery& q){std::lock_guard lock(mutex_);SessionListPage page;if(q.tenant_id.empty()||q.principal_id.empty()||q.limit==0)return page;auto query=std::string("SELECT ")+columns+" FROM product_sessions s JOIN product_session_members m ON m.tenant=s.tenant AND m.session_id=s.session_id WHERE s.tenant=? AND m.principal_id=?";if(q.state)query+=" AND s.state=?";if(!q.search.empty())query+=" AND (s.title LIKE ? OR s.tags_json LIKE ?)";if(q.before_sequence)query+=" AND s.sequence<?";query+=" ORDER BY s.sequence DESC LIMIT ?";sql::Statement stmt(sql::database(db_),query.c_str());int i=1;sql::bind_text(stmt.get(),i++,q.tenant_id);sql::bind_text(stmt.get(),i++,q.principal_id);if(q.state)sql::bind_text(stmt.get(),i++,name(*q.state));if(!q.search.empty()){auto pattern="%"+q.search+"%";sql::bind_text(stmt.get(),i++,pattern);sql::bind_text(stmt.get(),i++,pattern);}if(q.before_sequence)sql::bind_uint64(stmt.get(),i++,q.before_sequence);sql::bind_uint64(stmt.get(),i++,std::min<std::size_t>(q.limit,200));while(sql::step(stmt.get())==SQLITE_ROW)page.sessions.push_back(decode(stmt.get()));if(page.sessions.size()==std::min<std::size_t>(q.limit,200))page.next_before_sequence=page.sessions.back().sequence;return page;}
SessionMutationResult SQLiteSessionCatalog::rename(std::string_view tenant,std::string_view session,std::uint64_t expected,std::string_view title){if(title.empty()||title.size()>256)return {false,expected,"invalid_title"};std::lock_guard lock(mutex_);auto* db=sql::database(db_);sql::Statement q(db,"UPDATE product_sessions SET title=?,revision=revision+1,updated_at=? WHERE tenant=? AND session_id=? AND revision=? AND state!='purged'");sql::bind_text(q.get(),1,title);sql::bind_text(q.get(),2,stamp());sql::bind_text(q.get(),3,tenant);sql::bind_text(q.get(),4,session);sql::bind_uint64(q.get(),5,expected);if(sql::step(q.get())!=SQLITE_DONE)return {false,expected,sqlite3_errmsg(db)};return changed(db,expected);}
SessionMutationResult SQLiteSessionCatalog::organize(std::string_view tenant,std::string_view session,std::uint64_t expected,std::string_view folder,const std::vector<std::string>& tags,bool pinned){if(tags.size()>32)return {false,expected,"invalid_tags"};std::lock_guard lock(mutex_);auto* db=sql::database(db_);sql::Statement q(db,"UPDATE product_sessions SET folder=?,tags_json=?,pinned=?,revision=revision+1,updated_at=? WHERE tenant=? AND session_id=? AND revision=? AND state!='purged'");sql::bind_text(q.get(),1,folder);sql::bind_text(q.get(),2,nlohmann::json(tags).dump());sql::bind_int(q.get(),3,pinned?1:0);sql::bind_text(q.get(),4,stamp());sql::bind_text(q.get(),5,tenant);sql::bind_text(q.get(),6,session);sql::bind_uint64(q.get(),7,expected);if(sql::step(q.get())!=SQLITE_DONE)return {false,expected,sqlite3_errmsg(db)};return changed(db,expected);}
SessionMutationResult SQLiteSessionCatalog::transition(std::string_view tenant,std::string_view session,std::uint64_t expected,ProductSessionState target){std::lock_guard lock(mutex_);auto* db=sql::database(db_);sql::Statement find(db,"SELECT state,revision FROM product_sessions WHERE tenant=? AND session_id=?");sql::bind_text(find.get(),1,tenant);sql::bind_text(find.get(),2,session);if(sql::step(find.get())!=SQLITE_ROW)return {false,0,"resource_not_found"};auto current=*state_from(sql::column_text(find.get(),0));auto revision=sql::column_uint64(find.get(),1);if(revision!=expected)return {false,revision,"revision_conflict"};if(!transition_allowed(current,target))return {false,revision,"invalid_lifecycle_transition"};sql::Statement q(db,"UPDATE product_sessions SET state=?,revision=revision+1,updated_at=? WHERE tenant=? AND session_id=? AND revision=?");sql::bind_text(q.get(),1,name(target));sql::bind_text(q.get(),2,stamp());sql::bind_text(q.get(),3,tenant);sql::bind_text(q.get(),4,session);sql::bind_uint64(q.get(),5,expected);if(sql::step(q.get())!=SQLITE_DONE)return {false,expected,sqlite3_errmsg(db)};return changed(db,expected);}
SessionMutationResult SQLiteSessionCatalog::put_member(const SessionMember& m,std::uint64_t expected){if(m.tenant_id.empty()||m.session_id.empty()||m.principal_id.empty())return {false,0,"identity_required"};std::lock_guard lock(mutex_);auto* db=sql::database(db_);if(expected==0){sql::Statement q(db,"INSERT INTO product_session_members VALUES(?,?,?,?,1)");sql::bind_text(q.get(),1,m.tenant_id);sql::bind_text(q.get(),2,m.session_id);sql::bind_text(q.get(),3,m.principal_id);sql::bind_text(q.get(),4,name(m.role));if(sql::step(q.get())!=SQLITE_DONE)return {false,0,"idempotency_conflict"};return {true,1,{}};}sql::Statement q(db,"UPDATE product_session_members SET role=?,revision=revision+1 WHERE tenant=? AND session_id=? AND principal_id=? AND revision=?");sql::bind_text(q.get(),1,name(m.role));sql::bind_text(q.get(),2,m.tenant_id);sql::bind_text(q.get(),3,m.session_id);sql::bind_text(q.get(),4,m.principal_id);sql::bind_uint64(q.get(),5,expected);if(sql::step(q.get())!=SQLITE_DONE)return {false,expected,sqlite3_errmsg(db)};return changed(db,expected);}
} // namespace agent_framework::session
