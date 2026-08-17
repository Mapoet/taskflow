#include <cassert>
#include <filesystem>
#include <string>

#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_no_tls_client.hpp>

#include "agent/api/v1/websocket_gateway.hpp"

using namespace agent_framework;

namespace
{
    identity::RuntimeSubject subject()
    {
        identity::RuntimeSubject value;
        value.tenant_id = "tenant";
        value.organization_id = "org";
        value.principal_id = "owner";
        value.project_id = "project";
        value.workspace_id = "workspace";
        value.session_id = "request-session";
        value.conversation_id = "request-conversation";
        value.agent_id = "agent";
        value.authorization_revision = 1;
        value.authenticated = true;
        return value;
    }

    session::ProductSession product()
    {
        session::ProductSession value;
        value.tenant_id = "tenant";
        value.organization_id = "org";
        value.project_id = "project";
        value.workspace_id = "workspace";
        value.session_id = "session-1";
        value.conversation_id = "conversation-1";
        value.owner_principal_id = "owner";
        value.title = "WebSocket session";
        return value;
    }
}

int main()
{
    const auto root = std::filesystem::temp_directory_path() / "agent-session-run-websocket-api";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    session::SQLiteSessionCatalog catalog((root / "catalog.sqlite").string());
    session::SQLiteSessionRunSupervisor supervisor((root / "runs.sqlite").string());
    conversation::SQLiteConversationStore events((root / "events.sqlite").string());
    api::v1::SessionRunApi api(catalog, supervisor, &events);
    assert(api.create_session(subject(), product()).status == 201);

    conversation::RuntimeEventEnvelope event;
    event.event_id = "event-1";
    event.tenant_id = "tenant";
    event.conversation_id = "conversation-1";
    event.turn_id = "turn-1";
    event.run_id = "run-1";
    event.sequence = 1;
    event.durability = conversation::EventDurability::Durable;
    event.visibility = conversation::EventVisibility::User;
    event.event_type = "answer.delta";
    event.payload = {{"text", "hello"}};
    event.timestamp = "now";
    assert(events.append_event(event, nullptr));

    api::v1::WebSocketGatewayOptions options;
    options.port = 0;
    options.maximum_frame_bytes = 512;
    options.poll_interval = std::chrono::milliseconds(10);
    api::v1::WebSocketEventGateway gateway(api, [](std::string_view authorization) -> std::optional<identity::RuntimeSubject>
                                           {
        if(authorization=="Bearer test-token")return subject();return std::nullopt; }, options);
    std::string start_error;
    assert(gateway.start(&start_error));
    assert(gateway.port() != 0);

    using Client = websocketpp::client<websocketpp::config::asio_client>;
    Client client;
    client.clear_access_channels(websocketpp::log::alevel::all);
    client.clear_error_channels(websocketpp::log::elevel::all);
    client.init_asio();
    bool opened = false;
    bool received = false;
    bool failed = false;
    int protocol_stage = 0;
    client.set_open_handler([&](websocketpp::connection_hdl handle)
                            {
        opened=true;websocketpp::lib::error_code error;
        client.send(handle,R"({"type":"subscribe","topics":[{"session_id":"session-1","after":0}]})",
                    websocketpp::frame::opcode::text,error);assert(!error); });
    client.set_message_handler([&](websocketpp::connection_hdl handle, Client::message_ptr message)
                               {
        const auto body=nlohmann::json::parse(message->get_payload());
        websocketpp::lib::error_code error;
        assert(body.at("type")=="subscribed");
        if(protocol_stage==0) {
            assert(body.at("topics")==1);assert(body.at("frames").size()==1);
            assert(body.at("frames").at(0).at("cursor")==1);protocol_stage=1;
            client.send(handle,R"({"type":"update","topics":[{"session_id":"session-1","after":1}]})",
                        websocketpp::frame::opcode::text,error);
        } else if(protocol_stage==1) {
            assert(body.at("topics")==1);assert(body.at("frames").empty());protocol_stage=2;
            client.send(handle,R"({"type":"unsubscribe"})",websocketpp::frame::opcode::text,error);
        } else {
            assert(protocol_stage==2);assert(body.at("topics")==0);assert(body.at("frames").empty());
            protocol_stage=3;received=true;
            client.close(handle,websocketpp::close::status::normal,"complete",error);
        }
        assert(!error); });
    client.set_fail_handler([&](websocketpp::connection_hdl)
                            { failed = true; });
    websocketpp::lib::error_code connect_error;
    auto connection = client.get_connection("ws://127.0.0.1:" + std::to_string(gateway.port()) + "/api/v1/events",
                                            connect_error);
    assert(!connect_error);
    connection->append_header("Authorization", "Bearer test-token");
    client.connect(connection);
    client.run();
    assert(opened && !failed && received && protocol_stage == 3);

    Client oversized;
    oversized.clear_access_channels(websocketpp::log::alevel::all);
    oversized.clear_error_channels(websocketpp::log::elevel::all);
    oversized.init_asio();
    bool oversized_rejected = false;
    oversized.set_open_handler([&](websocketpp::connection_hdl handle) {
        websocketpp::lib::error_code error;
        oversized.send(handle,std::string(513,'x'),websocketpp::frame::opcode::text,error);
        assert(!error);
    });
    oversized.set_message_handler([&](websocketpp::connection_hdl handle,Client::message_ptr message) {
        const auto body=nlohmann::json::parse(message->get_payload());
        oversized_rejected=body.at("type")=="error"&&body.at("error")=="frame_too_large";
        websocketpp::lib::error_code error;
        oversized.close(handle,websocketpp::close::status::normal,"complete",error);
    });
    auto oversized_connection=oversized.get_connection(
        "ws://127.0.0.1:"+std::to_string(gateway.port())+"/api/v1/events",connect_error);
    assert(!connect_error);
    oversized_connection->append_header("Authorization","Bearer test-token");
    oversized.connect(oversized_connection);oversized.run();
    assert(oversized_rejected);

    Client unauthorized;
    unauthorized.clear_access_channels(websocketpp::log::alevel::all);
    unauthorized.clear_error_channels(websocketpp::log::elevel::all);
    unauthorized.init_asio();
    bool unauthorized_opened = false;
    bool unauthorized_failed = false;
    unauthorized.set_open_handler([&](websocketpp::connection_hdl)
                                  { unauthorized_opened = true; });
    unauthorized.set_fail_handler([&](websocketpp::connection_hdl)
                                  { unauthorized_failed = true; });
    auto rejected = unauthorized.get_connection(
        "ws://127.0.0.1:" + std::to_string(gateway.port()) + "/api/v1/events", connect_error);
    assert(!connect_error);
    unauthorized.connect(rejected);
    unauthorized.run();
    assert(!unauthorized_opened && unauthorized_failed);

    gateway.stop();
    std::filesystem::remove_all(root);
}
