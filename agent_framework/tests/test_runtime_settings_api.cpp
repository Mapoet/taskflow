#include <cassert>
#include <chrono>
#include <filesystem>

#include "agent/api/v1/runtime_settings_api.hpp"

using namespace agent_framework;

int main() {
    const auto path=std::filesystem::temp_directory_path()/(
        "agent-runtime-settings-test-"+std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count())+".sqlite3");
    identity::RuntimeSubject subject;
    subject.tenant_id="tenant";subject.organization_id="organization";
    subject.project_id="project";subject.workspace_id="workspace";
    subject.principal_id="owner";subject.agent_id="agent";
    subject.session_id="settings";subject.conversation_id="settings";
    subject.authenticated=true;subject.authorization_revision=7;
    api::v1::RuntimeSettingsStore store(path.string(),{
        {"provider.id","default"},{"provider.model","model"},{"provider.endpoint",""},
        {"provider.credentials_configured",true},{"mcp.enabled",true},{"mcp.registry","registry"},
        {"skills.enabled",true},{"skills.root","/skills"},{"sandbox.mode","strict"},
        {"sandbox.root","/workspace"},{"workspace.directory","/workspace"},
        {"planning.depth","bounded"},{"memory.strategy","adaptive"},
        {"assurance.tier","professional"},{"judge.mode","required"},
        {"logging.level","info"},{"logging.redaction",true},{"observability.enabled",true},
        {"appearance.theme","dark"},{"appearance.language","en"}});
    auto initial=store.snapshot(subject);assert(initial.ok());
    assert(initial.body.at("revision")==1&&initial.body.at("authorization_revision")==7);
    assert(initial.body.at("fields").size()==20);
    auto updated=store.update(subject,1,{{"planning.depth","continuous"},{"appearance.language","zh-CN"}});
    assert(updated.ok()&&updated.body.at("revision")==2&&updated.body.at("restart_required")==true);
    auto stale=store.update(subject,1,{{"logging.level","debug"}});
    assert(stale.status==409&&stale.body.at("revision")==2);
    auto readonly=store.update(subject,2,{{"provider.credentials_configured",false}});
    assert(readonly.status==422&&readonly.body.at("error")=="setting_is_read_only");
    auto unknown=store.update(subject,2,{{"provider.api_key","secret"}});
    assert(unknown.status==422&&unknown.body.at("error")=="unknown_setting");
    auto current=store.snapshot(subject);assert(current.body.at("revision")==2);
    bool language=false;for(const auto& field:current.body.at("fields"))
        if(field.at("key")=="appearance.language")language=field.at("value")=="zh-CN";
    assert(language);
    std::error_code ec;std::filesystem::remove(path,ec);
}
