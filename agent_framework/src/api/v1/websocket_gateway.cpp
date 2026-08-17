#include "agent/api/v1/websocket_gateway.hpp"

#include <atomic>
#include <map>
#include <mutex>
#include <thread>

#include <boost/system/error_code.hpp>

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

namespace agent_framework::api::v1 {
namespace {
using Server=websocketpp::server<websocketpp::config::asio>;
using Handle=websocketpp::connection_hdl;
using nlohmann::json;
struct ConnectionState {identity::RuntimeSubject subject;std::vector<EventTopicCursor> topics;};
}

struct WebSocketEventGateway::Impl {
    SessionRunApi& api;WebSocketSubjectResolver resolver;WebSocketGatewayOptions options;
    AuthorizedEventMultiplexer multiplexer;Server server;
    std::map<Handle,ConnectionState,std::owner_less<Handle>> connections;
    std::shared_ptr<websocketpp::lib::asio::steady_timer> timer;
    std::thread thread;std::uint16_t bound_port{0};std::atomic_bool running{false};
    mutable std::mutex lifecycle;

    Impl(SessionRunApi& value,WebSocketSubjectResolver auth,WebSocketGatewayOptions config)
        :api(value),resolver(std::move(auth)),options(std::move(config)),
         multiplexer(api,options.maximum_topics) {}

    bool validate(Handle handle) {
        auto connection=server.get_con_from_hdl(handle);
        auto subject=resolver?resolver(connection->get_request_header("Authorization")):std::nullopt;
        if(!subject||!identity::validate(*subject,identity::SubjectBoundary::Production).empty()) {
            connection->set_status(websocketpp::http::status_code::unauthorized);
            connection->set_body("authentication_required");return false;
        }
        connections[handle]={*subject,{}};return true;
    }
    void close(Handle handle) {connections.erase(handle);}
    void send(Handle handle,const json& value) {
        auto document=value.dump();websocketpp::lib::error_code error;
        auto connection=server.get_con_from_hdl(handle,error);if(error)return;
        if(document.size()>options.maximum_frame_bytes||
           connection->get_buffered_amount()>options.maximum_buffered_bytes) {
            server.close(handle,websocketpp::close::status::policy_violation,
                         "gateway_backpressure",error);return;
        }
        server.send(handle,document,websocketpp::frame::opcode::text,error);
    }
    void message(Handle handle,Server::message_ptr payload) {
        auto found=connections.find(handle);if(found==connections.end())return;
        try {
            if(payload->get_payload().size()>options.maximum_frame_bytes)
                throw std::invalid_argument("frame_too_large");
            const auto body=json::parse(payload->get_payload());
            const auto type=body.at("type").get<std::string>();
            if(type=="unsubscribe") {
                found->second.topics.clear();
                send(handle,{{"type","subscribed"},{"topics",0},{"frames",json::array()}});
                return;
            }
            if(type!="subscribe"&&type!="update")throw std::invalid_argument("unsupported_message_type");
            std::vector<EventTopicCursor> topics;
            for(const auto& topic:body.at("topics"))topics.push_back({topic.at("session_id"),topic.value("after",0U)});
            auto checked=multiplexer.poll(found->second.subject,topics,100);
            if(!checked.ok)throw std::invalid_argument(checked.error);
            found->second.topics=std::move(checked.cursors);
            send(handle,{{"type","subscribed"},{"topics",found->second.topics.size()},
                         {"frames",std::move(checked.frames)}});
        } catch(const std::exception& error) {send(handle,{{"type","error"},{"error",error.what()}});}
    }
    void poll() {
        for(auto& [handle,state]:connections) {
            if(state.topics.empty())continue;
            auto result=multiplexer.poll(state.subject,state.topics,100);
            if(!result.ok) {send(handle,{{"type","error"},{"error",result.error}});continue;}
            state.topics=std::move(result.cursors);
            if(!result.frames.empty())send(handle,{{"type","events"},{"frames",std::move(result.frames)}});
        }
        if(!running.load(std::memory_order_acquire))return;
        timer->expires_from_now(options.poll_interval);
        timer->async_wait([this](const websocketpp::lib::error_code& error){if(!error)poll();});
    }
    bool start(std::string* error) {
        std::lock_guard lock(lifecycle);if(running.load(std::memory_order_acquire))return true;
        try {
            server.clear_access_channels(websocketpp::log::alevel::all);
            server.clear_error_channels(websocketpp::log::elevel::all);
            server.init_asio();server.set_reuse_addr(true);
            server.set_validate_handler([this](Handle h){return validate(h);});
            server.set_close_handler([this](Handle h){close(h);});
            server.set_fail_handler([this](Handle h){close(h);});
            server.set_message_handler([this](Handle h,Server::message_ptr m){message(h,std::move(m));});
            websocketpp::lib::asio::ip::tcp::endpoint endpoint(
                websocketpp::lib::asio::ip::address::from_string(options.bind_address),options.port);
            server.listen(endpoint);boost::system::error_code endpoint_error;
            bound_port=server.get_local_endpoint(endpoint_error).port();
            if(endpoint_error)throw std::runtime_error(endpoint_error.message());
            server.start_accept();
            running.store(true,std::memory_order_release);
            timer=std::make_shared<websocketpp::lib::asio::steady_timer>(server.get_io_service());
            poll();thread=std::thread([this]{server.run();});return true;
        } catch(const std::exception& exception) {if(error)*error=exception.what();return false;}
    }
    void stop() {
        {std::lock_guard lock(lifecycle);
            if(!running.exchange(false,std::memory_order_acq_rel))return;}
        websocketpp::lib::error_code error;server.stop_listening(error);
        server.get_io_service().post([this]{
            websocketpp::lib::error_code ignored;
            for(const auto& item:connections)server.close(item.first,websocketpp::close::status::going_away,"shutdown",ignored);
            connections.clear();if(timer)timer->cancel();
        });
        if(thread.joinable())thread.join();
    }
};

WebSocketEventGateway::WebSocketEventGateway(SessionRunApi& api,WebSocketSubjectResolver resolver,
    WebSocketGatewayOptions options):impl_(std::make_unique<Impl>(api,std::move(resolver),std::move(options))){}
WebSocketEventGateway::~WebSocketEventGateway(){stop();}
bool WebSocketEventGateway::start(std::string* error){return impl_->start(error);}
void WebSocketEventGateway::stop(){impl_->stop();}
std::uint16_t WebSocketEventGateway::port()const{return impl_->bound_port;}

} // namespace agent_framework::api::v1
