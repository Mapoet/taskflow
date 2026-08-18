#include "agent/api/v1/runtime_settings_api.hpp"

#include <chrono>
#include <filesystem>
#include <map>

#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::api::v1 {
namespace {
using nlohmann::json;
using namespace agent_framework::internal::sqlite;

struct Database {
    sqlite3* value{nullptr};
    explicit Database(const std::string& path) {
        if(sqlite3_open_v2(path.c_str(), &value, SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,
                           nullptr)!=SQLITE_OK) {
            const std::string error=value?sqlite3_errmsg(value):"sqlite open failed";
            if(value)sqlite3_close(value);value=nullptr;throw std::runtime_error(error);
        }
        exec(value,"PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON; PRAGMA busy_timeout=3000;");
        exec(value,"CREATE TABLE IF NOT EXISTS runtime_settings("
                   "tenant TEXT PRIMARY KEY, revision INTEGER NOT NULL, values_json TEXT NOT NULL,"
                   "updated_by TEXT NOT NULL, updated_at TEXT NOT NULL);");
        exec(value,"CREATE TABLE IF NOT EXISTS runtime_settings_audit("
                   "tenant TEXT NOT NULL, revision INTEGER NOT NULL, actor TEXT NOT NULL,"
                   "updates_json TEXT NOT NULL, created_at TEXT NOT NULL,"
                   "PRIMARY KEY(tenant,revision));");
    }
    ~Database(){if(value)sqlite3_close(value);}
};

std::string now_ms() {
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

json descriptors() {
    return json::array({
        {{"key","provider.id"},{"category","Provider / Model"},{"label","Provider"},{"type","text"},{"mutability","restart_required"}},
        {{"key","provider.model"},{"category","Provider / Model"},{"label","Model"},{"type","text"},{"mutability","restart_required"}},
        {{"key","provider.endpoint"},{"category","Provider / Model"},{"label","Endpoint"},{"type","text"},{"mutability","restart_required"}},
        {{"key","provider.credentials_configured"},{"category","Provider / Model"},{"label","Credentials"},{"type","status"},{"mutability","read_only"}},
        {{"key","mcp.enabled"},{"category","MCP"},{"label","Enable MCP"},{"type","boolean"},{"mutability","restart_required"}},
        {{"key","mcp.registry"},{"category","MCP"},{"label","Registry source"},{"type","text"},{"mutability","restart_required"}},
        {{"key","skills.enabled"},{"category","Skills"},{"label","Enable Skills"},{"type","boolean"},{"mutability","restart_required"}},
        {{"key","skills.root"},{"category","Skills"},{"label","Skills root"},{"type","path"},{"mutability","restart_required"}},
        {{"key","sandbox.mode"},{"category","Tool sandbox"},{"label","Sandbox mode"},{"type","select"},{"options",{"strict","workspace","disabled"}},{"mutability","restart_required"}},
        {{"key","sandbox.root"},{"category","Tool sandbox"},{"label","Filesystem root"},{"type","path"},{"mutability","restart_required"}},
        {{"key","workspace.directory"},{"category","Workspace"},{"label","Working directory"},{"type","path"},{"mutability","restart_required"}},
        {{"key","planning.depth"},{"category","Planning"},{"label","Default planning depth"},{"type","select"},{"options",{"none","bounded","comprehensive","continuous"}},{"mutability","restart_required"}},
        {{"key","memory.strategy"},{"category","Memory"},{"label","Memory strategy"},{"type","select"},{"options",{"minimal","task","project","adaptive"}},{"mutability","restart_required"}},
        {{"key","assurance.tier"},{"category","Assurance / Judge"},{"label","Assurance tier"},{"type","select"},{"options",{"basic","functional","professional"}},{"mutability","restart_required"}},
        {{"key","judge.mode"},{"category","Assurance / Judge"},{"label","Judge mode"},{"type","select"},{"options",{"disabled","advisory","required"}},{"mutability","restart_required"}},
        {{"key","logging.level"},{"category","Logging / Observability"},{"label","Log level"},{"type","select"},{"options",{"error","warn","info","debug"}},{"mutability","restart_required"}},
        {{"key","logging.redaction"},{"category","Logging / Observability"},{"label","Sensitive-data redaction"},{"type","boolean"},{"mutability","restart_required"}},
        {{"key","observability.enabled"},{"category","Logging / Observability"},{"label","OTel observability"},{"type","boolean"},{"mutability","restart_required"}},
        {{"key","appearance.theme"},{"category","Appearance"},{"label","Theme"},{"type","select"},{"options",{"dark","system"}},{"mutability","dynamic"}},
        {{"key","appearance.language"},{"category","Appearance"},{"label","Language"},{"type","select"},{"options",{"en","zh-CN"}},{"mutability","dynamic"}}
    });
}

bool authorized(const identity::RuntimeSubject& subject) {
    return subject.authenticated && subject.authorization_revision>0 &&
        identity::validate(subject,identity::SubjectBoundary::Production).empty();
}

json read_values(sqlite3* db,std::string_view tenant,std::uint64_t& revision,
                 std::string& actor,std::string& updated_at) {
    Statement query(db,"SELECT revision,values_json,updated_by,updated_at FROM runtime_settings WHERE tenant=?");
    bind_text(query.get(),1,tenant);
    if(step(query.get())!=SQLITE_ROW){revision=1;return json::object();}
    revision=column_uint64(query.get(),0);actor=column_text(query.get(),2);updated_at=column_text(query.get(),3);
    return json::parse(column_text(query.get(),1));
}

std::optional<std::string> validate_update(const json& descriptor,const json& value) {
    const auto mutability=descriptor.at("mutability").get<std::string>();
    if(mutability=="read_only")return "setting_is_read_only";
    const auto type=descriptor.at("type").get<std::string>();
    if(type=="boolean"&&!value.is_boolean())return "setting_boolean_required";
    if((type=="text"||type=="path"||type=="select")&&!value.is_string())return "setting_string_required";
    if(type=="path"&&value.get<std::string>().find('\0')!=std::string::npos)return "setting_path_invalid";
    if(type=="select") {
        bool found=false;for(const auto& option:descriptor.at("options"))if(option==value){found=true;break;}
        if(!found)return "setting_option_invalid";
    }
    return {};
}
} // namespace

RuntimeSettingsStore::RuntimeSettingsStore(std::string path,json defaults)
    :path_(std::move(path)),defaults_(std::move(defaults)) {
    if(path_.empty())throw std::invalid_argument("runtime settings database path required");
    std::error_code ec;const std::filesystem::path file(path_);
    if(file.has_parent_path())std::filesystem::create_directories(file.parent_path(),ec);
    Database database(path_);
}

ApiResult RuntimeSettingsStore::snapshot(const identity::RuntimeSubject& subject) const {
    if(!authorized(subject))return {401,{{"error","production_identity_required"}}};
    std::lock_guard lock(mutex_);Database database(path_);std::uint64_t revision=1;
    std::string actor,updated_at;auto values=read_values(database.value,subject.tenant_id,revision,actor,updated_at);
    for(auto it=defaults_.begin();it!=defaults_.end();++it)if(!values.contains(it.key()))values[it.key()]=it.value();
    auto fields=descriptors();for(auto& field:fields)field["value"]=values.value(field.at("key").get<std::string>(),json{});
    return {200,{{"schema","agent.runtime_settings/v1"},{"revision",revision},
        {"authorization_revision",subject.authorization_revision},{"updated_by",actor},
        {"updated_at",updated_at},{"fields",std::move(fields)}}};
}

ApiResult RuntimeSettingsStore::update(const identity::RuntimeSubject& subject,
                                       std::uint64_t expected,const json& updates) {
    if(!authorized(subject))return {401,{{"error","production_identity_required"}}};
    if(!updates.is_object()||updates.empty())return {422,{{"error","settings_updates_required"}}};
    std::map<std::string,json> definitions;for(const auto& d:descriptors())definitions.emplace(d.at("key"),d);
    bool restart_required=false;
    for(auto it=updates.begin();it!=updates.end();++it) {
        auto found=definitions.find(it.key());if(found==definitions.end())return {422,{{"error","unknown_setting"},{"key",it.key()}}};
        if(auto error=validate_update(found->second,it.value()))return {422,{{"error",*error},{"key",it.key()}}};
        restart_required=restart_required||found->second.at("mutability")=="restart_required";
    }
    std::lock_guard lock(mutex_);Database database(path_);Transaction transaction(database.value);
    std::uint64_t revision=1;std::string actor,updated_at;auto values=read_values(database.value,subject.tenant_id,revision,actor,updated_at);
    if(expected!=revision)return {409,{{"error","settings_revision_conflict"},{"revision",revision},
        {"safe_retry","refresh_and_reapply_if_intent_still_valid"}}};
    for(auto it=updates.begin();it!=updates.end();++it)values[it.key()]=it.value();
    const auto next=revision+1;const auto timestamp=now_ms();
    Statement save(database.value,"INSERT INTO runtime_settings(tenant,revision,values_json,updated_by,updated_at) VALUES(?,?,?,?,?) "
        "ON CONFLICT(tenant) DO UPDATE SET revision=excluded.revision,values_json=excluded.values_json,updated_by=excluded.updated_by,updated_at=excluded.updated_at");
    bind_text(save.get(),1,subject.tenant_id);bind_uint64(save.get(),2,next);bind_text(save.get(),3,values.dump());
    bind_text(save.get(),4,subject.principal_id);bind_text(save.get(),5,timestamp);
    if(step(save.get())!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(database.value));
    Statement audit(database.value,"INSERT INTO runtime_settings_audit(tenant,revision,actor,updates_json,created_at) VALUES(?,?,?,?,?)");
    bind_text(audit.get(),1,subject.tenant_id);bind_uint64(audit.get(),2,next);bind_text(audit.get(),3,subject.principal_id);
    bind_text(audit.get(),4,updates.dump());bind_text(audit.get(),5,timestamp);
    if(step(audit.get())!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(database.value));
    transaction.commit();return {200,{{"updated",true},{"revision",next},{"restart_required",restart_required}}};
}

void register_runtime_settings_routes(httplib::Server& server,RuntimeSettingsStore& store,
                                      RuntimeSubjectResolver resolver) {
    auto send=[](httplib::Response& response,const ApiResult& result){response.status=result.status;
        response.set_header("Cache-Control","no-store");response.set_content(result.body.dump(),"application/json");};
    server.Get("/api/v1/me",[resolver,send](const auto& request,auto& response){
        auto subject=resolver?resolver(request):std::nullopt;if(!subject){send(response,{401,{{"error","authentication_required"}}});return;}
        send(response,{200,{{"schema","agent.runtime_subject_profile/v1"},{"tenant_id",subject->tenant_id},
            {"organization_id",subject->organization_id},{"project_id",subject->project_id},
            {"workspace_id",subject->workspace_id},{"principal_id",subject->principal_id},
            {"agent_id",subject->agent_id},{"authenticated",subject->authenticated},
            {"authorization_revision",subject->authorization_revision}}});});
    server.Get("/api/v1/runtime/settings",[&store,resolver,send](const auto& request,auto& response){
        auto subject=resolver?resolver(request):std::nullopt;if(!subject){send(response,{401,{{"error","authentication_required"}}});return;}
        send(response,store.snapshot(*subject));});
    server.Post("/api/v1/runtime/settings",[&store,resolver,send](const auto& request,auto& response){
        auto subject=resolver?resolver(request):std::nullopt;if(!subject){send(response,{401,{{"error","authentication_required"}}});return;}
        try{auto body=json::parse(request.body);send(response,store.update(*subject,body.at("expected_revision"),body.at("updates")));}
        catch(const std::exception& error){send(response,{400,{{"error","invalid_request"},{"detail",error.what()}}});}});
}

} // namespace agent_framework::api::v1
