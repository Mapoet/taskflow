#pragma once
#include "agent/conversation/types.hpp"
namespace agent_framework::conversation
{
    class TurnStateMachine
    {
    public:
        static bool transition(TurnCheckpoint &, TurnPhase, TurnContinuationReason,
                               std::string *error = nullptr);
        static bool terminal(TurnPhase);
    };
}
