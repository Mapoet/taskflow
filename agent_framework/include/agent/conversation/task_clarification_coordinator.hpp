#pragma once

#include "agent/conversation/conversation_engine.hpp"
#include "agent/conversation/task_orchestrator.hpp"
#include "agent/conversation/task_profile_clarification.hpp"

namespace agent_framework::conversation {

struct ClarificationResumeResult {
    bool handled{false};
    bool resumed{false};
    TurnResult turn;
    std::string error;
};

class TaskClarificationCoordinator {
public:
    TaskClarificationCoordinator(ConversationStore& conversations,
                                 TaskProfileClarificationStore& clarifications,
                                 TaskRegistry& tasks)
        : conversations_(conversations), clarifications_(clarifications), tasks_(tasks) {}

    TurnResult begin(const TurnRequest&, TaskInputIntent,
                     const TaskClassification&, std::uint64_t now_ms,
                     std::uint64_t ttl_ms, RuntimeEventSink = {});
    ClarificationResumeResult answer_pending(const ConversationIdentity&,
        std::string_view answer, std::uint64_t now_ms, TurnExecutor,
        RuntimeEventSink = {});

private:
    ConversationStore& conversations_;
    TaskProfileClarificationStore& clarifications_;
    TaskRegistry& tasks_;
};

}  // namespace agent_framework::conversation
