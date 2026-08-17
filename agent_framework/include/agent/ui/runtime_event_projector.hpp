#pragma once

#include <cstddef>
#include <string>

#include "agent/conversation/store.hpp"
#include "agent/ui/interaction_projection_store.hpp"

namespace agent_framework::ui {

struct RuntimeProjectionResult {
    bool ok{false};
    std::uint64_t runtime_head{0},projection_head{0},projection_revision{0};
    std::size_t applied{0};
    std::string error;
};

// Rebuildable adapter from the canonical Conversation event stream. One
// projection event is committed for every runtime sequence, including events
// hidden from the current viewer, so Conversation and Observation share a
// single cursor without leaking hidden payloads.
class RuntimeEventInteractionProjector {
public:
    RuntimeEventInteractionProjector(conversation::ConversationStore& source,
                                     InteractionProjectionStore& target)
        :source_(source),target_(target) {}
    RuntimeProjectionResult synchronize(const conversation::ConversationIdentity&,
                                        std::size_t batch_size=256);
private:
    conversation::ConversationStore& source_;
    InteractionProjectionStore& target_;
};

} // namespace agent_framework::ui
