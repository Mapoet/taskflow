#include "agent/api/v1/http_routes.hpp"

#include <chrono>
#include <thread>

namespace agent_framework::api::v1 {
namespace {
using nlohmann::json;

void send(httplib::Response& response,const ApiResult& result) {
    response.status=result.status;
    response.set_header("Cache-Control","no-store");
    response.set_content(result.body.dump(),"application/json");
}

std::optional<identity::RuntimeSubject> resolve(const httplib::Request& request,
                                                const RuntimeSubjectResolver& resolver,
                                                httplib::Response& response) {
    auto subject=resolver?resolver(request):std::nullopt;
    if(!subject) send(response,{401,{{"error","authentication_required"}}});
    return subject;
}

ApiResult malformed(const std::exception& error) {
    return {400,{{"error","invalid_request"},{"detail",error.what()}}};
}

std::optional<session::ProductSessionState> session_state(std::string_view value) {
    for(std::size_t i=0;i<5;++i) {
        auto state=static_cast<session::ProductSessionState>(i);
        if(session::name(state)==value) return state;
    }
    return {};
}

std::optional<session::SessionCommandKind> command_kind(std::string_view value) {
    for(std::size_t i=0;i<9;++i) {
        auto kind=static_cast<session::SessionCommandKind>(i);
        if(session::name(kind)==value) return kind;
    }
    return {};
}
std::optional<session::SessionMemberRole> member_role(std::string_view value) {
    for(std::size_t i=0;i<4;++i){auto role=static_cast<session::SessionMemberRole>(i);
        if(session::name(role)==value)return role;}return {};
}
} // namespace

void register_session_run_routes(httplib::Server& server,SessionRunApi& api,
                                 RuntimeSubjectResolver resolver) {
    server.Get("/api/v1/sessions",[&api,resolver](const auto& request,auto& response) {
        auto subject=resolve(request,resolver,response);if(!subject)return;
        try {
            session::SessionListQuery query;
            if(request.has_param("search"))query.search=request.get_param_value("search");
            if(request.has_param("state")) {
                const auto requested=request.get_param_value("state");
                if(requested=="all")query.state.reset();
                else {
                    auto parsed=session_state(requested);
                    if(!parsed){send(response,{422,{{"error","invalid_session_state"}}});return;}
                    query.state=*parsed;
                }
            }
            if(request.has_param("before"))query.before_sequence=std::stoull(request.get_param_value("before"));
            if(request.has_param("limit"))query.limit=std::stoull(request.get_param_value("limit"));
            send(response,api.list_sessions(*subject,std::move(query)));
        } catch(const std::exception& error) { send(response,malformed(error)); }
    });
    server.Get(R"(/api/v1/sessions/([^/]+))",[&api,resolver](const auto& request,auto& response) {
        auto subject=resolve(request,resolver,response);if(!subject)return;
        send(response,api.get_session(*subject,request.matches[1].str()));
    });
    server.Get(R"(/api/v1/sessions/([^/]+)/events)",[&api,resolver](const auto& request,auto& response) {
        auto subject=resolve(request,resolver,response);if(!subject)return;
        try {
            std::uint64_t after=0;std::size_t limit=100;
            if(request.has_param("after"))after=std::stoull(request.get_param_value("after"));
            if(request.has_param("limit"))limit=std::stoull(request.get_param_value("limit"));
            send(response,api.replay_events(*subject,request.matches[1].str(),after,limit));
        } catch(const std::exception& error) { send(response,malformed(error)); }
    });
    server.Get(R"(/api/v1/sessions/([^/]+)/events/stream)",[&api,resolver](const auto& request,auto& response) {
        auto subject=resolve(request,resolver,response);if(!subject)return;
        std::uint64_t after=0;
        try {
            const auto last=request.get_header_value("Last-Event-ID");
            if(!last.empty())after=std::stoull(last);
            else if(request.has_param("after"))after=std::stoull(request.get_param_value("after"));
        } catch(const std::exception& error) {send(response,malformed(error));return;}
        const auto session_id=request.matches[1].str();
        auto initial=api.replay_events(*subject,session_id,after,100);
        if(!initial.ok()) {send(response,initial);return;}
        response.status=200;response.set_header("Cache-Control","no-cache");
        response.set_header("Connection","keep-alive");response.set_header("X-Accel-Buffering","no");
        response.set_chunked_content_provider("text/event-stream",
            [&api,subject=*subject,session_id,cursor=after,
             heartbeat=std::chrono::steady_clock::now()](std::size_t,httplib::DataSink& sink) mutable {
                if(!sink.is_writable())return false;
                auto page=api.replay_events(subject,session_id,cursor,100);
                if(!page.ok()) {
                    const auto payload=std::string("event: gateway.error\ndata: ")+page.body.dump()+"\n\n";
                    sink.write(payload.data(),payload.size());return false;
                }
                std::string chunk;
                for(const auto& event:page.body.at("items")) {
                    chunk+="id: "+std::to_string(event.at("sequence").template get<std::uint64_t>())+"\n";
                    chunk+=std::string("event: ")+event.at("event_type").template get<std::string>()+"\n";
                    chunk+="data: "+event.dump()+"\n\n";
                }
                cursor=page.body.at("next_cursor").template get<std::uint64_t>();
                if(!chunk.empty()) {sink.write(chunk.data(),chunk.size());heartbeat=std::chrono::steady_clock::now();}
                else if(std::chrono::steady_clock::now()-heartbeat>=std::chrono::seconds(5)) {
                    static constexpr char ping[]=": heartbeat\n\n";sink.write(ping,sizeof(ping)-1);
                    heartbeat=std::chrono::steady_clock::now();
                } else std::this_thread::sleep_for(std::chrono::milliseconds(25));
                return sink.is_writable();
            },[]{});
    });
    server.Get(R"(/api/v1/sessions/([^/]+)/artifacts/([^/]+))",[&api,resolver](const auto& request,auto& response) {
        auto subject=resolve(request,resolver,response);if(!subject)return;
        send(response,api.get_artifact(*subject,request.matches[1].str(),request.matches[2].str()));
    });
    server.Get(R"(/api/v1/sessions/([^/]+)/interactions)",[&api,resolver](const auto& request,auto& response) {
        auto subject=resolve(request,resolver,response);if(!subject)return;
        auto viewer=ui::InteractionVisibility::User;
        if(request.has_param("visibility")) {
            auto parsed=ui::interaction_visibility(request.get_param_value("visibility"));
            if(!parsed){send(response,{422,{{"error","invalid_interaction_visibility"}}});return;}
            viewer=*parsed;
        }
        send(response,api.get_interactions(*subject,request.matches[1].str(),viewer));
    });
    server.Get(R"(/api/v1/sessions/([^/]+)/approvals/([^/]+))",[&api,resolver](const auto& request,auto& response) {
        auto subject=resolve(request,resolver,response);if(!subject)return;
        send(response,api.get_approval(*subject,request.matches[1].str(),request.matches[2].str()));
    });
    server.Get(R"(/api/v1/sessions/([^/]+)/decisions/([^/]+))",[&api,resolver](const auto& request,auto& response) {
        auto subject=resolve(request,resolver,response);if(!subject)return;
        send(response,api.get_decision(*subject,request.matches[1].str(),request.matches[2].str()));
    });
    server.Get(R"(/api/v1/sessions/([^/]+)/tasks/([^/]+)/execution-snapshot)",[&api,resolver](const auto& request,auto& response) {
        auto subject=resolve(request,resolver,response);if(!subject)return;
        const auto run=request.has_param("run_id")?request.get_param_value("run_id"):std::string{};
        send(response,api.get_execution_snapshot(*subject,request.matches[1].str(),
                                                  request.matches[2].str(),run));
    });
    server.Get(R"(/api/v1/sessions/([^/]+)/tasks/([^/]+))",[&api,resolver](const auto& request,auto& response) {
        auto subject=resolve(request,resolver,response);if(!subject)return;
        send(response,api.get_task(*subject,request.matches[1].str(),request.matches[2].str()));
    });
    server.Get(R"(/api/v1/sessions/([^/]+)/tasks/([^/]+)/semantics)",[&api,resolver](const auto& request,auto& response) {
        auto subject=resolve(request,resolver,response);if(!subject)return;
        send(response,api.get_task_semantics(*subject,request.matches[1].str(),request.matches[2].str()));
    });
    server.Get(R"(/api/v1/sessions/([^/]+)/tasks/([^/]+)/plans/([^/]+))",[&api,resolver](const auto& request,auto& response) {
        auto subject=resolve(request,resolver,response);if(!subject)return;
        send(response,api.get_plan(*subject,request.matches[1].str(),request.matches[2].str(),request.matches[3].str()));
    });
    server.Get(R"(/api/v1/sessions/([^/]+)/tasks/([^/]+)/observations)",[&api,resolver](const auto& request,auto& response) {
        auto subject=resolve(request,resolver,response);if(!subject)return;
        send(response,api.get_observations(*subject,request.matches[1].str(),request.matches[2].str()));
    });
    server.Get(R"(/api/v1/sessions/([^/]+)/tasks/([^/]+)/evidence/([^/]+))",[&api,resolver](const auto& request,auto& response) {
        auto subject=resolve(request,resolver,response);if(!subject)return;
        send(response,api.get_evidence(*subject,request.matches[1].str(),request.matches[2].str(),request.matches[3].str()));
    });
    server.Post(R"(/api/v1/sessions/([^/]+)/decisions/([^/]+)/answer)",[&api,resolver](const auto& request,auto& response) {
        auto subject=resolve(request,resolver,response);if(!subject)return;
        try {const auto body=json::parse(request.body);
            const auto now=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            send(response,api.answer_decision(*subject,request.matches[1].str(),request.matches[2].str(),
                body.at("expected_revision"),body.at("option_id").template get<std::string>(),now));
        } catch(const std::exception& error){send(response,malformed(error));}
    });
    server.Get(R"(/api/v1/sessions/([^/]+)/capabilities)",[&api,resolver](const auto& request,auto& response) {
        auto subject=resolve(request,resolver,response);if(!subject)return;
        send(response,api.capabilities(*subject,request.matches[1].str()));
    });
    server.Post("/api/v1/sessions",[&api,resolver](const auto& request,auto& response) {
        auto subject=resolve(request,resolver,response);if(!subject)return;
        try {
            auto body=json::parse(request.body);session::ProductSession value;
            value.tenant_id=subject->tenant_id;value.organization_id=subject->organization_id;
            value.project_id=subject->project_id;value.workspace_id=subject->workspace_id;
            value.owner_principal_id=subject->principal_id;
            value.session_id=body.at("session_id");value.conversation_id=body.at("conversation_id");
            value.title=body.at("title");value.folder=body.value("folder","");
            value.tags=body.value("tags",std::vector<std::string>{});value.pinned=body.value("pinned",false);
            send(response,api.create_session(*subject,std::move(value)));
        } catch(const std::exception& error) { send(response,malformed(error)); }
    });
    server.Post(R"(/api/v1/sessions/([^/]+)/fork)",[&api,resolver](const auto& request,auto& response) {
        auto subject=resolve(request,resolver,response);if(!subject)return;
        try {auto body=json::parse(request.body);session::ProductSession forked;
            forked.session_id=body.at("session_id");forked.conversation_id=body.at("conversation_id");
            forked.title=body.value("title",std::string{});forked.folder=body.value("folder",std::string{});
            forked.tags=body.value("tags",std::vector<std::string>{});forked.pinned=body.value("pinned",false);
            send(response,api.fork_session(*subject,request.matches[1].str(),
                body.at("expected_source_revision"),std::move(forked)));}
        catch(const std::exception& error){send(response,malformed(error));}
    });
    server.Post(R"(/api/v1/sessions/([^/]+)/state)",[&api,resolver](const auto& request,auto& response) {
        auto subject=resolve(request,resolver,response);if(!subject)return;
        try {
            auto body=json::parse(request.body);auto state=session_state(body.at("state").template get<std::string>());
            if(!state) {send(response,{422,{{"error","invalid_session_state"}}});return;}
            send(response,api.transition_session(*subject,request.matches[1].str(),
                                                  body.at("expected_revision"),*state));
        } catch(const std::exception& error) { send(response,malformed(error)); }
    });
    server.Post(R"(/api/v1/sessions/([^/]+)/restore)",[&api,resolver](const auto& request,auto& response) {
        auto subject=resolve(request,resolver,response);if(!subject)return;
        try {auto body=json::parse(request.body);send(response,api.restore_session(*subject,
            request.matches[1].str(),body.at("expected_revision")));}
        catch(const std::exception& error){send(response,malformed(error));}
    });
    server.Post(R"(/api/v1/sessions/([^/]+)/purge)",[&api,resolver](const auto& request,auto& response) {
        auto subject=resolve(request,resolver,response);if(!subject)return;
        try {auto body=json::parse(request.body);send(response,api.purge_session(*subject,
            request.matches[1].str(),body.at("expected_revision"),
            body.value("confirm_permanent",false)));}
        catch(const std::exception& error){send(response,malformed(error));}
    });
    server.Get(R"(/api/v1/sessions/([^/]+)/data)",[&api,resolver](const auto& request,auto& response) {
        auto subject=resolve(request,resolver,response);if(!subject)return;
        try {const auto after=request.has_param("after")?std::stoull(request.get_param_value("after")):0;
            const auto limit=request.has_param("limit")?std::stoull(request.get_param_value("limit")):200;
            send(response,api.get_session_data(*subject,request.matches[1].str(),after,limit));}
        catch(const std::exception& error){send(response,malformed(error));}
    });
    server.Post(R"(/api/v1/sessions/([^/]+)/title)",[&api,resolver](const auto& request,auto& response) {
        auto subject=resolve(request,resolver,response);if(!subject)return;
        try {auto body=json::parse(request.body);send(response,api.rename_session(*subject,
            request.matches[1].str(),body.at("expected_revision"),body.at("title").template get<std::string>()));}
        catch(const std::exception& error){send(response,malformed(error));}
    });
    server.Post(R"(/api/v1/sessions/([^/]+)/organization)",[&api,resolver](const auto& request,auto& response) {
        auto subject=resolve(request,resolver,response);if(!subject)return;
        try {auto body=json::parse(request.body);send(response,api.organize_session(*subject,
            request.matches[1].str(),body.at("expected_revision"),body.value("folder",std::string{}),
            body.value("tags",std::vector<std::string>{}),body.value("pinned",false)));}
        catch(const std::exception& error){send(response,malformed(error));}
    });
    server.Post(R"(/api/v1/sessions/([^/]+)/members/([^/]+))",[&api,resolver](const auto& request,auto& response) {
        auto subject=resolve(request,resolver,response);if(!subject)return;
        try {auto body=json::parse(request.body);auto parsed=member_role(body.at("role").template get<std::string>());
            if(!parsed){send(response,{422,{{"error","invalid_session_member_role"}}});return;}
            send(response,api.put_session_member(*subject,request.matches[1].str(),
                body.value("expected_member_revision",0ULL),request.matches[2].str(),*parsed));}
        catch(const std::exception& error){send(response,malformed(error));}
    });
    server.Post("/api/v1/runs",[&api,resolver](const auto& request,auto& response) {
        auto subject=resolve(request,resolver,response);if(!subject)return;
        try {
            auto body=json::parse(request.body);session::SessionRunRequest value;
            value.tenant_id=subject->tenant_id;value.organization_id=subject->organization_id;
            value.project_id=subject->project_id;value.principal_id=subject->principal_id;
            value.provider_id=body.at("provider_id");value.session_id=body.at("session_id");
            value.run_id=body.at("run_id");value.command_id=body.at("command_id");
            value.payload=body.value("payload",json::object());
            send(response,api.enqueue_run(*subject,std::move(value)));
        } catch(const std::exception& error) { send(response,malformed(error)); }
    });
    server.Get(R"(/api/v1/runs/([^/]+))",[&api,resolver](const auto& request,auto& response) {
        auto subject=resolve(request,resolver,response);if(!subject)return;
        send(response,api.get_run(*subject,request.matches[1].str()));
    });
    server.Post(R"(/api/v1/runs/([^/]+)/commands)",[&api,resolver](const auto& request,auto& response) {
        auto subject=resolve(request,resolver,response);if(!subject)return;
        try {
            auto body=json::parse(request.body);auto kind=command_kind(body.at("kind").template get<std::string>());
            if(!kind||*kind==session::SessionCommandKind::Start) {
                send(response,{422,{{"error","invalid_command_kind"}}});return;
            }
            session::SessionRunCommand value;value.tenant_id=subject->tenant_id;
            value.session_id=body.at("session_id");value.run_id=request.matches[1].str();
            value.command_id=body.at("command_id");value.kind=*kind;
            value.payload=body.value("payload",json::object());
            if(body.contains("expected_run_revision"))
                value.expected_run_revision=body.at("expected_run_revision").template get<std::uint64_t>();
            send(response,api.enqueue_command(*subject,std::move(value)));
        } catch(const std::exception& error) { send(response,malformed(error)); }
    });
}

} // namespace agent_framework::api::v1
