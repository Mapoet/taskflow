#include "agent/tool_runtime/state_machine.hpp"
#include <initializer_list>
namespace agent_framework::tool_runtime
{
    bool can_transition(InvocationState f, InvocationState t) noexcept
    {
        if (f == t)
            return false;
        auto one = [&](std::initializer_list<InvocationState> x)
        {for(auto v:x)if(v==t)return true;return false; };
        switch (f)
        {
        case InvocationState::Created:
            return one({InvocationState::Admitted, InvocationState::Failed, InvocationState::Cancelled});
        case InvocationState::Admitted:
            return one({InvocationState::Queued, InvocationState::Failed, InvocationState::Cancelled});
        case InvocationState::Queued:
            return one({InvocationState::Leased, InvocationState::Cancelled, InvocationState::ManualReview});
        case InvocationState::Leased:
            return one({InvocationState::Running, InvocationState::Queued, InvocationState::Orphaned, InvocationState::Cancelling});
        case InvocationState::Running:
        case InvocationState::Progressing:
        case InvocationState::Checkpointed:
            return one({InvocationState::Progressing, InvocationState::Checkpointed, InvocationState::AwaitingInput, InvocationState::AwaitingApproval, InvocationState::CompletedCandidate, InvocationState::Retrying, InvocationState::Reconciling, InvocationState::Cancelling, InvocationState::Failed, InvocationState::Orphaned});
        case InvocationState::AwaitingInput:
        case InvocationState::AwaitingApproval:
            return one({InvocationState::Queued, InvocationState::Cancelling, InvocationState::Cancelled});
        case InvocationState::Cancelling:
            return one({InvocationState::Cancelled, InvocationState::Reconciling, InvocationState::ManualReview});
        case InvocationState::Retrying:
            return one({InvocationState::Queued, InvocationState::Failed, InvocationState::ManualReview});
        case InvocationState::Reconciling:
            return one({InvocationState::EffectCommitted, InvocationState::Retrying, InvocationState::ManualReview, InvocationState::Failed});
        case InvocationState::CompletedCandidate:
            return one({InvocationState::EffectCommitted, InvocationState::Reconciling, InvocationState::Failed, InvocationState::ManualReview});
        case InvocationState::EffectCommitted:
            return one({InvocationState::Verified, InvocationState::Failed});
        case InvocationState::Orphaned:
            return one({InvocationState::Queued, InvocationState::Reconciling, InvocationState::ManualReview});
        default:
            return false;
        }
    }
}
