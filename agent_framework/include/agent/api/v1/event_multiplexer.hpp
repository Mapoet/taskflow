#pragma once

#include <string>
#include <vector>

#include "agent/api/v1/session_run_api.hpp"

namespace agent_framework::api::v1 {

struct EventTopicCursor { std::string session_id;std::uint64_t after{0}; };
struct EventMultiplexResult {
    bool ok{false};
    std::vector<EventTopicCursor> cursors;
    nlohmann::json frames=nlohmann::json::array();
    std::string error;
};

class AuthorizedEventMultiplexer {
public:
    explicit AuthorizedEventMultiplexer(SessionRunApi& api,std::size_t maximum_topics=32)
        : api_(api),maximum_topics_(maximum_topics) {}
    EventMultiplexResult poll(const identity::RuntimeSubject&,
        const std::vector<EventTopicCursor>&,std::size_t limit_per_topic=100);
private:
    SessionRunApi& api_;std::size_t maximum_topics_;
};

} // namespace agent_framework::api::v1
