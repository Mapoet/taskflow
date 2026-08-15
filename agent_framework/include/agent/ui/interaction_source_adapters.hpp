#pragma once

#include <string>

#include "agent/ui/interaction_graph.hpp"
#include "agent/ui/phase4_operations.hpp"

namespace agent_framework::ui {

struct InteractionProjectionContext {
    std::string conversation_id{"default"};
    std::string turn_id{"turn-current"};
    std::string user_message_id{"message-current"};
    std::string user_message_summary;
};

// Converts canonical, display-safe control-plane state into navigable UI objects.
// It never reads private prompt bodies, memory contents, credentials or chain-of-thought.
InteractionSnapshot project_interactions(const Phase4OperationsSnapshot&,
                                         const InteractionProjectionContext&);

} // namespace agent_framework::ui
