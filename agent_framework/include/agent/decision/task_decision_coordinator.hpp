#pragma once

#include "agent/conversation/conversation_engine.hpp"
#include "agent/conversation/task_classifier.hpp"
#include "agent/conversation/task_orchestrator.hpp"
#include "agent/conversation/task_profile_clarification.hpp"
#include "agent/decision/decision_store.hpp"

namespace agent_framework::decision {

struct DecisionResumeResult {
    bool handled{false};
    bool resumed{false};
    conversation::TurnResult turn;
    std::string error;
};

// One-way compatibility migration. The legacy row is cancelled only after the
// general Decision has been durably created, so restart cannot lose the wait.
bool migrate_profile_clarification(const conversation::TaskProfileClarification&,
    const identity::RuntimeSubject&,DecisionStore&,
    conversation::TaskProfileClarificationStore&,std::string* error=nullptr);

class TaskDecisionCoordinator {
public:
    TaskDecisionCoordinator(conversation::ConversationStore& conversations,
        DecisionStore& decisions,conversation::TaskRegistry& tasks)
        :conversations_(conversations),decisions_(decisions),tasks_(tasks){}

    conversation::TurnResult begin(const identity::RuntimeSubject&,
        const conversation::TurnRequest&,conversation::TaskInputIntent,
        const conversation::TaskClassification&,std::uint64_t now_ms,
        std::uint64_t ttl_ms,conversation::RuntimeEventSink={});
    DecisionResumeResult answer_pending(const identity::RuntimeSubject&,
        std::string_view option_id,std::uint64_t now_ms,
        conversation::TurnExecutor,conversation::RuntimeEventSink={});

private:
    conversation::ConversationStore& conversations_;
    DecisionStore& decisions_;
    conversation::TaskRegistry& tasks_;
};

}  // namespace agent_framework::decision
