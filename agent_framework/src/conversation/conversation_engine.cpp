#include "agent/conversation/conversation_engine.hpp"
#include "agent/contracts/contract.hpp"
#include <chrono>
namespace agent_framework::conversation
{
    namespace
    {
        std::string stamp() { return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()); }
        TurnPhase phase_for(ModelTurnStopReason r)
        {
            switch (r)
            {
            case ModelTurnStopReason::ToolRequested:
                return TurnPhase::AwaitingTool;
            case ModelTurnStopReason::EndTurn:
                return TurnPhase::Completed;
            case ModelTurnStopReason::Cancelled:
                return TurnPhase::Interrupted;
            default:
                return TurnPhase::Failed;
            }
        }
    }
    void ConversationEngine::event(const TurnRequest &r, TurnCheckpoint &c, std::string type, nlohmann::json payload, EventDurability d, EventVisibility v)
    {
        (void)c;
        const auto prior = store_.last_event_sequence(r.identity);
        RuntimeEventEnvelope e;
        e.event_id = r.turn_id + ":" + std::to_string(prior + 1);
        e.tenant_id = r.identity.tenant_id;
        e.conversation_id = r.identity.conversation_id;
        e.turn_id = r.turn_id;
        e.run_id = r.turn_id;
        e.sequence = prior + 1;
        e.durability = d;
        e.visibility = v;
        e.event_type = std::move(type);
        e.timestamp = stamp();
        e.payload = std::move(payload);
        std::string ignored;
        if (d == EventDurability::Durable)
            store_.append_event(e, &ignored);
        if (sink_)
            sink_(e);
    }
    TurnResult ConversationEngine::start_turn(const TurnRequest &r)
    {
        if (auto e = validate(r); !e.empty())
            return {{}, {}, e.front()};
        if (store_.load_turn(r.identity, r.turn_id))
            return {{}, {}, "turn_already_exists"};
        TurnCheckpoint c;
        c.identity = r.identity;
        c.turn_id = r.turn_id;
        std::string err;
        if (!TurnStateMachine::transition(c, TurnPhase::Running, TurnContinuationReason::InitialRequest, &err))
            return {c, {}, err};
        auto prior = store_.messages(r.identity);
        ConversationMessage m;
        m.identity = r.identity;
        m.message_id = r.turn_id + ":user";
        m.parent_id = prior.empty() ? "" : prior.back().message_id;
        m.turn_id = r.turn_id;
        m.role = "user";
        m.content = r.input;
        m.created_at = stamp();
        RuntimeEventEnvelope started;
        started.tenant_id = r.identity.tenant_id;
        started.conversation_id = r.identity.conversation_id;
        started.turn_id = r.turn_id;
        started.run_id = r.turn_id;
        started.durability = EventDurability::Durable;
        started.visibility = EventVisibility::Operations;
        started.event_type = "turn_started";
        started.timestamp = stamp();
        started.payload = {{"profile", name(r.profile)}};
        ConversationCommit initial{c, 0, {m}, {started}, {}};
        if (!store_.commit(initial, &err))
            return {c, {}, err};
        c = initial.checkpoint;
        if (sink_)
            sink_(initial.durable_events.front());
        return execute(r, c);
    }
    TurnResult ConversationEngine::continue_turn(const TurnRequest &r, TurnContinuationReason why)
    {
        auto c = store_.load_turn(r.identity, r.turn_id);
        if (!c)
            return {{}, {}, "turn_not_found"};
        std::string err;
        auto expected = c->revision;
        if (!TurnStateMachine::transition(*c, TurnPhase::Running, why, &err))
            return {*c, {}, err};
        if (!store_.commit_turn(*c, expected, &err))
            return {*c, {}, err};
        return execute(r, *c);
    }
    TurnResult ConversationEngine::resume_turn(const TurnRequest &r, TurnContinuationReason why) { return continue_turn(r, why); }
    TurnResult ConversationEngine::execute(const TurnRequest &r, TurnCheckpoint c)
    {
        auto expected = c.revision;
        ++c.iteration;
        event(r, c, "model_request", {{"iteration", c.iteration}}, EventDurability::Ephemeral, EventVisibility::Internal);
        ModelTurnOutcome o;
        try
        {
            o = executor_(r, c);
        }
        catch (const std::exception &e)
        {
            o.reason = ModelTurnStopReason::ProviderError;
            o.candidate_answer = e.what();
        }
        if (auto problems = validate(o); !problems.empty())
        {
            o.reason = ModelTurnStopReason::ProviderError;
            o.task_completion_verified = false;
            o.candidate_answer = problems.front();
        }
        auto next = phase_for(o.reason);
        std::string err;
        if (!TurnStateMachine::transition(c, next, next == TurnPhase::AwaitingTool ? TurnContinuationReason::ToolResultsAvailable : TurnContinuationReason::None, &err))
            return {c, o, err};
        std::vector<ConversationMessage> pending_messages;
        if (!o.candidate_answer.empty())
        {
            auto prior = store_.messages(r.identity);
            ConversationMessage m;
            m.identity = r.identity;
            m.message_id = r.turn_id + ":assistant:" + std::to_string(c.iteration);
            m.parent_id = prior.empty() ? "" : prior.back().message_id;
            m.turn_id = r.turn_id;
            m.role = "assistant";
            m.content = o.candidate_answer;
            m.created_at = stamp();
            pending_messages.push_back(std::move(m));
        }
        RuntimeEventEnvelope stopped;
        stopped.tenant_id = r.identity.tenant_id;
        stopped.conversation_id = r.identity.conversation_id;
        stopped.turn_id = r.turn_id;
        stopped.run_id = r.turn_id;
        stopped.durability = EventDurability::Durable;
        stopped.visibility = EventVisibility::User;
        stopped.event_type = "model_stop";
        stopped.timestamp = stamp();
        stopped.payload = {{"reason", name(o.reason)}, {"task_completion_verified", false}};
        ConversationCommit terminal{c, expected, std::move(pending_messages), {stopped}, {}};
        if (!store_.commit(terminal, &err))
            return {c, o, err};
        c = terminal.checkpoint;
        if (sink_)
            sink_(terminal.durable_events.front());
        return {c, o, {}};
    }
    bool ConversationEngine::interrupt_turn(const ConversationIdentity &i, std::string_view id, std::string *e)
    {
        auto c = store_.load_turn(i, id);
        if (!c)
        {
            if (e)
                *e = "turn_not_found";
            return false;
        }
        auto expected = c->revision;
        if (!TurnStateMachine::transition(*c, TurnPhase::Interrupted, TurnContinuationReason::None, e))
            return false;
        RuntimeEventEnvelope interrupted;
        interrupted.turn_id = std::string(id);
        interrupted.run_id = std::string(id);
        interrupted.durability = EventDurability::Durable;
        interrupted.visibility = EventVisibility::User;
        interrupted.event_type = "turn_interrupted";
        interrupted.timestamp = stamp();
        ConversationCommit commit{*c, expected, {}, {interrupted}, {}};
        if (!store_.commit(commit, e))
            return false;
        if (sink_)
            sink_(commit.durable_events.front());
        return true;
    }
    InputDisposition ConversationEngine::classify_input(std::string_view v) const
    {
        if (v.rfind("/cancel", 0) == 0 || v.rfind("/stop", 0) == 0)
            return InputDisposition::ControlAction;
        if (v.rfind("/status", 0) == 0)
            return InputDisposition::StatusQuery;
        if (v.rfind("/replace ", 0) == 0)
            return InputDisposition::InterruptAndReplace;
        if (v.rfind("/next ", 0) == 0)
            return InputDisposition::QueueNextTurn;
        return InputDisposition::AppendToCurrentTurn;
    }
    bool ConversationEngine::submit_user_input(const TurnRequest &r, InputDisposition d, std::string *e)
    {
        if (d == InputDisposition::StatusQuery)
            return true;
        auto c = store_.load_turn(r.identity, r.turn_id);
        if (!c)
        {
            if (e)
                *e = "turn_not_found";
            return false;
        }
        const auto expected = c->revision;
        if (d == InputDisposition::ControlAction || d == InputDisposition::InterruptAndReplace)
        {
            if (!TurnStateMachine::transition(*c, TurnPhase::Interrupted,
                                              TurnContinuationReason::None, e))
                return false;
        }
        else
        {
            ++c->revision;
            c->continuation = TurnContinuationReason::QueuedUserInput;
        }
        auto prior = store_.messages(r.identity);
        ConversationInput input;
        input.identity = r.identity;
        input.input_id = r.turn_id + ":input:" + std::to_string(expected + 1);
        input.target_turn_id = r.turn_id;
        input.content = r.input;
        input.created_at = stamp();
        input.disposition = d;
        input.state = d == InputDisposition::AppendToCurrentTurn
            ? InputState::Consumed : InputState::Queued;

        std::vector<ConversationMessage> messages;
        if (d == InputDisposition::AppendToCurrentTurn)
        {
            ConversationMessage message;
            message.identity = r.identity;
            message.message_id = input.input_id + ":message";
            message.parent_id = prior.empty() ? "" : prior.back().message_id;
            message.turn_id = r.turn_id;
            message.role = "user";
            message.content = r.input;
            message.created_at = input.created_at;
            messages.push_back(std::move(message));
        }
        RuntimeEventEnvelope event;
        event.turn_id = r.turn_id;
        event.run_id = r.turn_id;
        event.durability = EventDurability::Durable;
        event.visibility = EventVisibility::Operations;
        event.event_type = "user_input_queued";
        event.timestamp = input.created_at;
        event.payload = {{"disposition", name(d)}, {"state", name(input.state)}};
        ConversationCommit commit{*c, expected, std::move(messages), {event}, {input}};
        if (!store_.commit(commit, e))
            return false;
        if (sink_)
            sink_(commit.durable_events.front());
        return true;
    }
}
