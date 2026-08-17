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

    session::SessionMember op{"tenant","s1","operator",session::SessionMemberRole::Operator};
    session::SessionMember outsider{"tenant","s1","viewer",session::SessionMemberRole::Viewer};
    assert(catalog.put_member(op,0).ok&&catalog.put_member(outsider,0).ok);
    httplib::Headers operator_headers{{"Authorization","Bearer test"},{"X-Test-Principal","operator"}};
    httplib::Headers viewer_headers{{"Authorization","Bearer test"},{"X-Test-Principal","viewer"}};
    const char* run=R"({"provider_id":"provider","session_id":"s1","run_id":"r1","command_id":"start-1","payload":{"prompt":"work"}})";
    auto accepted=client.Post("/api/v1/runs",operator_headers,run,"application/json");
    assert(accepted&&accepted->status==202);
    auto replay=client.Post("/api/v1/runs",operator_headers,run,"application/json");
    assert(replay&&replay->status==202);
    auto forbidden=client.Post("/api/v1/runs",viewer_headers,run,"application/json");
    assert(forbidden&&forbidden->status==403);
    auto command=client.Post("/api/v1/runs/r1/commands",operator_headers,
        R"({"session_id":"s1","command_id":"cancel-1","kind":"cancel"})","application/json");
    assert(command&&command->status==202);
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
    std::string streamed;
    client.Get("/api/v1/sessions/s1/events/stream",viewer_headers,
        [&](const char* data,std::size_t size) {
            streamed.append(data,size);
            return streamed.find("id: 1\nevent: run.updated\n") == std::string::npos;
        });
    assert(streamed.find("id: 1\nevent: run.updated\n")!=std::string::npos);

    server.stop();listener.join();std::filesystem::remove_all(root);
}
