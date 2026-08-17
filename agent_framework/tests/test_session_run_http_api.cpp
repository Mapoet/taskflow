#include <cassert>
#include <filesystem>
#include <thread>

#include "agent/api/v1/http_routes.hpp"

using namespace agent_framework;

int main() {
    const auto root=std::filesystem::temp_directory_path()/"agent-session-run-http-api";
    std::filesystem::remove_all(root);std::filesystem::create_directories(root);
    session::SQLiteSessionCatalog catalog((root/"catalog.sqlite").string());
    session::SQLiteSessionRunSupervisor supervisor((root/"runs.sqlite").string());
    conversation::SQLiteConversationStore events((root/"events.sqlite").string());
    api::v1::SessionRunApi api(catalog,supervisor,&events);httplib::Server server;
    api::v1::register_session_run_routes(server,api,[](const httplib::Request& request)
        ->std::optional<identity::RuntimeSubject> {
        if(!request.has_header("Authorization")||request.get_header_value("Authorization")!="Bearer test")return {};
        identity::RuntimeSubject value;value.tenant_id="tenant";value.organization_id="org";
        value.principal_id=request.get_header_value("X-Test-Principal");value.project_id="project";
        value.workspace_id="workspace";value.session_id="request-session";
        value.conversation_id="request-conversation";value.agent_id="agent";
        value.authorization_revision=1;value.authenticated=true;return value;
    });
    const int port=server.bind_to_any_port("127.0.0.1");assert(port>0);
    std::thread listener([&]{server.listen_after_bind();});
    httplib::Client client("127.0.0.1",port);
    httplib::Headers owner{{"Authorization","Bearer test"},{"X-Test-Principal","owner"}};
    auto unauthorized=client.Get("/api/v1/sessions");assert(unauthorized&&unauthorized->status==401);
    auto created=client.Post("/api/v1/sessions",owner,
        R"({"session_id":"s1","conversation_id":"c1","title":"HTTP session"})","application/json");
    assert(created&&created->status==201);
    auto listed=client.Get("/api/v1/sessions",owner);assert(listed&&listed->status==200);
    assert(nlohmann::json::parse(listed->body).at("items").size()==1);
    auto manifest=client.Get("/api/v1/sessions/s1/capabilities",owner);
    assert(manifest&&manifest->status==200&&nlohmann::json::parse(manifest->body).at("schema")=="agent.capability_manifest/v1");

    auto renamed=client.Post("/api/v1/sessions/s1/title",owner,
        R"({"expected_revision":1,"title":"Renamed HTTP session"})","application/json");
    assert(renamed&&renamed->status==200&&nlohmann::json::parse(renamed->body).at("revision")==2);
    auto organized=client.Post("/api/v1/sessions/s1/organization",owner,
        R"({"expected_revision":2,"folder":"production","tags":["critical"],"pinned":true})","application/json");
    assert(organized&&organized->status==200&&nlohmann::json::parse(organized->body).at("revision")==3);
    auto stale_rename=client.Post("/api/v1/sessions/s1/title",owner,
        R"({"expected_revision":1,"title":"Stale"})","application/json");
    assert(stale_rename&&stale_rename->status==409);
    auto stale_rename_body=nlohmann::json::parse(stale_rename->body);
    assert(stale_rename_body.at("changed_fields").at(0)=="session_revision"&&
           stale_rename_body.at("safe_retry")=="refresh_and_reapply_if_intent_still_valid");
    auto invalid_member=client.Post("/api/v1/sessions/s1/members/invalid",owner,
        R"({"role":"root"})","application/json");
    assert(invalid_member&&invalid_member->status==422);
    auto member=client.Post("/api/v1/sessions/s1/members/http-operator",owner,
        R"({"role":"operator","expected_member_revision":0})","application/json");
    assert(member&&member->status==200);
    auto forked=client.Post("/api/v1/sessions/s1/fork",owner,
        R"({"expected_source_revision":3,"session_id":"s1-fork","conversation_id":"c1-fork"})",
        "application/json");
    assert(forked&&forked->status==201);
    const auto forked_body=nlohmann::json::parse(forked->body);
    assert(forked_body.at("source_revision")==3&&forked_body.at("revision")==1);
    auto stale_fork=client.Post("/api/v1/sessions/s1/fork",owner,
        R"({"expected_source_revision":2,"session_id":"stale-fork","conversation_id":"stale-conversation"})",
        "application/json");
    assert(stale_fork&&stale_fork->status==409);

    session::SessionMember op{"tenant","s1","operator",session::SessionMemberRole::Operator};
    session::SessionMember outsider{"tenant","s1","viewer",session::SessionMemberRole::Viewer};
    assert(catalog.put_member(op,0).ok&&catalog.put_member(outsider,0).ok);
    httplib::Headers operator_headers{{"Authorization","Bearer test"},{"X-Test-Principal","operator"}};
    httplib::Headers viewer_headers{{"Authorization","Bearer test"},{"X-Test-Principal","viewer"}};
    auto forbidden_rename=client.Post("/api/v1/sessions/s1/title",viewer_headers,
        R"({"expected_revision":3,"title":"Forbidden"})","application/json");
    assert(forbidden_rename&&forbidden_rename->status==403);
    const char* run=R"({"provider_id":"provider","session_id":"s1","run_id":"r1","command_id":"start-1","payload":{"prompt":"work"}})";
    auto accepted=client.Post("/api/v1/runs",operator_headers,run,"application/json");
    assert(accepted&&accepted->status==202);
    auto replay=client.Post("/api/v1/runs",operator_headers,run,"application/json");
    assert(replay&&replay->status==202);
    auto forbidden=client.Post("/api/v1/runs",viewer_headers,run,"application/json");
    assert(forbidden&&forbidden->status==403);
    auto command=client.Post("/api/v1/runs/r1/commands",operator_headers,
        R"({"session_id":"s1","command_id":"cancel-1","kind":"cancel","expected_run_revision":1})","application/json");
    assert(command&&command->status==202);
    auto stale=client.Post("/api/v1/runs/r1/commands",operator_headers,
        R"({"session_id":"s1","command_id":"stale","kind":"steer","expected_run_revision":0})","application/json");
    assert(stale&&stale->status==409);
    auto stale_body=nlohmann::json::parse(stale->body);
    assert(stale_body.at("revision")==1&&stale_body.at("changed_fields").at(0)=="run_revision"&&
           stale_body.at("safe_retry")=="refresh_and_reapply_if_intent_still_valid");
    auto premature_retry=client.Post("/api/v1/runs/r1/commands",operator_headers,
        R"({"session_id":"s1","command_id":"retry-1","kind":"retry","expected_run_revision":1})","application/json");
    assert(premature_retry&&premature_retry->status==422&&
           nlohmann::json::parse(premature_retry->body).at("error")=="run_command_invalid_for_state");
    auto bad=client.Post("/api/v1/runs/r1/commands",operator_headers,
        R"({"session_id":"s1","command_id":"bad","kind":"start"})","application/json");
    assert(bad&&bad->status==422);
    auto visible=client.Get("/api/v1/runs/r1",viewer_headers);assert(visible&&visible->status==200);
    conversation::RuntimeEventEnvelope event;event.event_id="event-1";event.tenant_id="tenant";
    event.conversation_id="c1";event.turn_id="turn";event.run_id="r1";event.sequence=1;
    event.durability=conversation::EventDurability::Durable;event.visibility=conversation::EventVisibility::User;
    event.event_type="run.updated";event.timestamp="now";assert(events.append_event(event,nullptr));
    auto replay_events=client.Get("/api/v1/sessions/s1/events?after=0&limit=10",viewer_headers);
    assert(replay_events&&replay_events->status==200&&nlohmann::json::parse(replay_events->body).at("items").size()==1);
    auto session_data=client.Get("/api/v1/sessions/s1/data?after=0&limit=10",viewer_headers);
    assert(session_data&&session_data->status==200);
    auto data_body=nlohmann::json::parse(session_data->body);
    assert(data_body.at("schema")=="agent.session_data_page/v1"&&
           data_body.at("events").at("items").size()==1);

    auto lifecycle=client.Post("/api/v1/sessions",owner,
        R"({"session_id":"s-lifecycle","conversation_id":"c-lifecycle","title":"Lifecycle"})","application/json");
    assert(lifecycle&&lifecycle->status==201);
    auto archived=client.Post("/api/v1/sessions/s-lifecycle/state",owner,
        R"({"expected_revision":1,"state":"archived"})","application/json");
    assert(archived&&archived->status==200);
    auto restored=client.Post("/api/v1/sessions/s-lifecycle/restore",owner,
        R"({"expected_revision":2})","application/json");
    assert(restored&&restored->status==200&&nlohmann::json::parse(restored->body).at("revision")==3);
    auto trashed=client.Post("/api/v1/sessions/s-lifecycle/state",owner,
        R"({"expected_revision":3,"state":"trashed"})","application/json");
    assert(trashed&&trashed->status==200);
    auto purge_pending=client.Post("/api/v1/sessions/s-lifecycle/purge",owner,
        R"({"expected_revision":4,"confirm_permanent":false})","application/json");
    assert(purge_pending&&purge_pending->status==200);
    auto purged=client.Post("/api/v1/sessions/s-lifecycle/purge",owner,
        R"({"expected_revision":5,"confirm_permanent":true})","application/json");
    assert(purged&&purged->status==200&&nlohmann::json::parse(purged->body).at("revision")==6);
    std::string streamed;
    client.Get("/api/v1/sessions/s1/events/stream",viewer_headers,
        [&](const char* data,std::size_t size) {
            streamed.append(data,size);
            return streamed.find("id: 1\nevent: run.updated\n") == std::string::npos;
        });
    assert(streamed.find("id: 1\nevent: run.updated\n")!=std::string::npos);

    server.stop();listener.join();std::filesystem::remove_all(root);
}
