#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "agent/api/v1/event_multiplexer.hpp"

namespace agent_framework::api::v1 {

struct WebSocketGatewayOptions {
    std::string bind_address{"127.0.0.1"};
    std::uint16_t port{0};
    std::size_t maximum_topics{32};
    std::size_t maximum_frame_bytes{1024U*1024U};
    std::size_t maximum_buffered_bytes{4U*1024U*1024U};
    std::chrono::milliseconds poll_interval{100};
};

using WebSocketSubjectResolver=std::function<std::optional<identity::RuntimeSubject>(
    std::string_view authorization)>;

class WebSocketEventGateway {
public:
    WebSocketEventGateway(SessionRunApi&,WebSocketSubjectResolver,
                          WebSocketGatewayOptions={});
    ~WebSocketEventGateway();
    WebSocketEventGateway(const WebSocketEventGateway&)=delete;
    WebSocketEventGateway& operator=(const WebSocketEventGateway&)=delete;
    bool start(std::string* error=nullptr);
    void stop();
    std::uint16_t port() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};

} // namespace agent_framework::api::v1
