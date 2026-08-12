#include "agent/conversation/turn_state_machine.hpp"
#include <set>
namespace agent_framework::conversation
{
    bool TurnStateMachine::terminal(TurnPhase p) { return p == TurnPhase::Completed || p == TurnPhase::Failed; }
    bool TurnStateMachine::transition(TurnCheckpoint &c, TurnPhase next, TurnContinuationReason why, std::string *e)
    {
        if (terminal(c.phase))
        {
            if (e)
                *e = "terminal_turn_cannot_transition";
            return false;
        }
        const std::set<std::pair<TurnPhase, TurnPhase>> allowed = {{TurnPhase::Pending, TurnPhase::Running}, {TurnPhase::Running, TurnPhase::AwaitingTool}, {TurnPhase::Running, TurnPhase::AwaitingInput}, {TurnPhase::Running, TurnPhase::Interrupted}, {TurnPhase::Running, TurnPhase::Completed}, {TurnPhase::Running, TurnPhase::Failed}, {TurnPhase::AwaitingTool, TurnPhase::Running}, {TurnPhase::AwaitingInput, TurnPhase::Running}, {TurnPhase::Interrupted, TurnPhase::Running}, {TurnPhase::AwaitingTool, TurnPhase::Interrupted}, {TurnPhase::AwaitingInput, TurnPhase::Interrupted}};
        if (!allowed.count({c.phase, next}))
        {
            if (e)
                *e = "invalid_turn_transition";
            return false;
        }
        c.phase = next;
        c.continuation = why;
        ++c.revision;
        return true;
    }
}
