#include "agent/api/v1/event_multiplexer.hpp"

#include <set>

namespace agent_framework::api::v1 {

EventMultiplexResult AuthorizedEventMultiplexer::poll(
    const identity::RuntimeSubject& subject,const std::vector<EventTopicCursor>& topics,
    std::size_t limit) {
    EventMultiplexResult out;
    if(topics.empty()||topics.size()>maximum_topics_||limit==0||limit>500) {
        out.error="invalid_multiplex_contract";return out;
    }
    std::set<std::string> unique;
    for(const auto& topic:topics) {
        if(topic.session_id.empty()||!unique.insert(topic.session_id).second) {
            out.error="duplicate_or_empty_event_topic";return out;
        }
        auto page=api_.replay_events(subject,topic.session_id,topic.after,limit);
        if(!page.ok()) {
            out.error=page.body.value("error","event_topic_failed");return out;
        }
        const auto next=page.body.at("next_cursor").get<std::uint64_t>();
        out.cursors.push_back({topic.session_id,next});
        for(const auto& event:page.body.at("items"))
            out.frames.push_back({{"session_id",topic.session_id},{"cursor",event.at("sequence")},
                                  {"event",event}});
    }
    out.ok=true;return out;
}

} // namespace agent_framework::api::v1
