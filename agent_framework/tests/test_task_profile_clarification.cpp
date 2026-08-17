#include <cassert>
#include <filesystem>
#include <string>
#include <barrier>
#include <thread>

#include "agent/conversation/task_profile_clarification.hpp"

using namespace agent_framework;
using namespace agent_framework::conversation;

int main() {
    for(const auto token : {"conversation", "read_only_analysis", "artifact_delivery",
                            "code_change", "external_action", "professional"})
        assert(parse_profile_confirmation(token));
    for(const auto invalid : {"不要 external_action", "professional conversation",
                              "please use professional", "professional.", "", "专业"})
        assert(!parse_profile_confirmation(invalid));

    const auto path = (std::filesystem::temp_directory_path() /
                       "agent-task-profile-clarification-test.sqlite").string();
    std::filesystem::remove(path);
    const ConversationIdentity identity{"tenant", "conversation"};
    TaskProfileClarification value;
    value.identity = identity;
    value.clarification_id = "clarification-1";
    value.task_id = "task-1";
    value.decision_id = "decision-1";
    value.turn_id = "turn-1";
    value.run_id = "run-1";
    value.recommended_profile = TaskExecutionProfile::Professional;
    value.allowed_tokens = {"conversation", "professional"};
    value.max_attempts = 3;
    value.expires_at_ms = 5000;
    value.created_at = "1000";
    value.updated_at = "1000";

    {
        SQLiteTaskProfileClarificationStore store(path);
        auto created = store.create(value);
        assert(created.ok && created.revision == 1);
        assert(store.create(value).ok);  // exact idempotent replay
        assert(store.pending(identity));
        auto bad1 = store.answer(identity, value.clarification_id, 1,
                                 "please use professional", 2000);
        assert(!bad1.ok && bad1.revision == 2 &&
               bad1.state == ProfileClarificationState::Pending);
        auto bad2 = store.answer(identity, value.clarification_id, 2,
                                 "destructive", 2001);
        assert(!bad2.ok && bad2.revision == 3);
    }
    {
        SQLiteTaskProfileClarificationStore reopened(path);
        auto recovered = reopened.pending(identity);
        assert(recovered && recovered->attempt_count == 2 && recovered->revision == 3);
        auto confirmed = reopened.answer(identity, value.clarification_id, 3,
                                         " professional ", 2002);
        assert(confirmed.ok && confirmed.state == ProfileClarificationState::Confirmed);
        auto replay = reopened.answer(identity, value.clarification_id, 3,
                                      "professional", 2003);
        assert(!replay.ok && replay.error == "clarification_revision_conflict");
        assert(!reopened.pending(identity));
    }

    TaskProfileClarification expired = value;
    expired.clarification_id = "clarification-expired";
    expired.decision_id = "decision-expired";
    expired.expires_at_ms = 10;
    {
        SQLiteTaskProfileClarificationStore store(path);
        assert(store.create(expired).ok);
        auto result = store.answer(identity, expired.clarification_id, 1,
                                   "professional", 10);
        assert(!result.ok && result.state == ProfileClarificationState::Expired);
    }

    TaskProfileClarification exhausted = value;
    exhausted.clarification_id = "clarification-exhausted";
    exhausted.decision_id = "decision-exhausted";
    exhausted.max_attempts = 1;
    {
        SQLiteTaskProfileClarificationStore store(path);
        assert(store.create(exhausted).ok);
        auto result = store.answer(identity, exhausted.clarification_id, 1, "no", 2);
        assert(!result.ok && result.state == ProfileClarificationState::Exhausted);
    }
    TaskProfileClarification raced = value;
    raced.clarification_id = "clarification-race";
    raced.decision_id = "decision-race";
    {
        SQLiteTaskProfileClarificationStore setup(path);
        assert(setup.create(raced).ok);
    }
    {
        SQLiteTaskProfileClarificationStore first(path), second(path);
        ClarificationMutationResult a,b;std::barrier ready(3);
        std::thread one([&]{ready.arrive_and_wait();a=first.answer(
            identity,raced.clarification_id,1,"professional",3);});
        std::thread two([&]{ready.arrive_and_wait();b=second.answer(
            identity,raced.clarification_id,1,"conversation",3);});
        ready.arrive_and_wait();one.join();two.join();
        assert(static_cast<int>(a.ok)+static_cast<int>(b.ok)==1);
    }
    std::filesystem::remove(path);
}
