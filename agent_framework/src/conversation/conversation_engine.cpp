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
            case ModelTurnStopReason::AwaitingExternal:
                return TurnPhase::AwaitingTool;
            case ModelTurnStopReason::AwaitingInput:
            case ModelTurnStopReason::AwaitingApproval:
                return TurnPhase::AwaitingInput;
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
        e.run_id = r.run_id.empty() ? r.turn_id : r.run_id;
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
        started.run_id = r.run_id.empty() ? r.turn_id : r.run_id;
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
        if (events_)
            events_->publish(initial.durable_events.front());
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
        std::vector<std::string> contract_problems = validate(o);
        if (!contract_problems.empty())
        {
            o.reason = ModelTurnStopReason::ProviderError;
            o.task_completion_verified = false;
            // Preserve an answer that may already have been streamed.  Adapter
            // contract failures belong to Operations/Audit, not user content.
            event(r, c, "turn_executor_contract_violation",
                  {{"issues", contract_problems}}, EventDurability::Durable,
                  EventVisibility::Operations);
        }
        auto next = phase_for(o.reason);
        std::string err;
        const auto continuation = o.reason == ModelTurnStopReason::AwaitingApproval
            ? TurnContinuationReason::ResumeAfterApproval
            : next == TurnPhase::AwaitingTool
                ? TurnContinuationReason::ToolResultsAvailable
                : TurnContinuationReason::None;
        if (!TurnStateMachine::transition(c, next, continuation, &err))
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
        stopped.run_id = r.run_id.empty() ? r.turn_id : r.run_id;
        stopped.durability = EventDurability::Durable;
        stopped.visibility = EventVisibility::User;
        stopped.event_type = "model_stop";
        stopped.timestamp = stamp();
        stopped.payload = {{"reason", name(o.reason)},
                           {"answer_present", !o.candidate_answer.empty()},
                           {"task_completion_verified", false}};
        if (!contract_problems.empty())
            stopped.payload["contract_violation"] = true;
        ConversationCommit terminal{c, expected, std::move(pending_messages), {stopped}, {}};
        if (!store_.commit(terminal, &err))
            return {c, o, err};
        c = terminal.checkpoint;
        if (sink_)
            sink_(terminal.durable_events.front());
        if (events_)
            events_->publish(terminal.durable_events.front());
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
        if (events_)
            events_->publish(commit.durable_events.front());
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
        TurnRequest normalized = r;
        if (d == InputDisposition::QueueNextTurn && normalized.input.rfind("/next ", 0) == 0)
            normalized.input.erase(0, 6);
        if (d == InputDisposition::InterruptAndReplace && normalized.input.rfind("/replace ", 0) == 0)
            normalized.input.erase(0, 9);
        if ((d == InputDisposition::QueueNextTurn || d == InputDisposition::InterruptAndReplace) &&
            normalized.input.find_first_not_of(" \t\r\n") == std::string::npos)
        {
            if (e) *e = "queued_input_payload_required";
            return false;
        }
        auto c = store_.load_turn(normalized.identity, normalized.turn_id);
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
        auto prior = store_.messages(normalized.identity);
        ConversationInput input;
        input.identity = normalized.identity;
        input.input_id = normalized.turn_id + ":input:" + std::to_string(expected + 1);
        input.target_turn_id = normalized.turn_id;
        input.content = normalized.input;
        input.created_at = stamp();
        input.disposition = d;
        input.profile = normalized.profile;
        input.max_iterations = normalized.max_iterations;
        input.max_input_tokens = normalized.max_input_tokens;
        input.max_output_tokens = normalized.max_output_tokens;
        input.state = d == InputDisposition::AppendToCurrentTurn
            ? InputState::Consumed : InputState::Queued;

        std::vector<ConversationMessage> messages;
        if (d == InputDisposition::AppendToCurrentTurn)
        {
            ConversationMessage message;
            message.identity = normalized.identity;
            message.message_id = input.input_id + ":message";
            message.parent_id = prior.empty() ? "" : prior.back().message_id;
            message.turn_id = normalized.turn_id;
            message.role = "user";
            message.content = normalized.input;
            message.created_at = input.created_at;
            messages.push_back(std::move(message));
        }
        RuntimeEventEnvelope event;
        event.turn_id = normalized.turn_id;
        event.run_id = normalized.run_id.empty() ? normalized.turn_id : normalized.run_id;
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
        if (events_)
            events_->publish(commit.durable_events.front());
        return true;
    }
    TurnResult ConversationEngine::start_next_queued_turn(const ConversationIdentity &identity)
    {
        std::string error;
        auto claim = store_.consume_next_queued_input(identity, &error);
        if (!claim)
            return {{}, {}, error.empty() ? "no_queued_input" : error};
        for (const auto &event : claim->durable_events)
        {
            if (sink_) sink_(event);
            if (events_) events_->publish(event);
        }
        TurnRequest request;
        request.identity = identity;
        request.turn_id = claim->checkpoint.turn_id;
        request.input = claim->input.content;
        request.profile = claim->input.profile;
        request.max_iterations = claim->input.max_iterations;
        request.max_input_tokens = claim->input.max_input_tokens;
        request.max_output_tokens = claim->input.max_output_tokens;
        return execute(request, claim->checkpoint);
    }
    std::vector<TurnResult> ConversationEngine::drain_queued_turns(
        const ConversationIdentity &identity, std::size_t limit)
    {
        std::vector<TurnResult> results;
        for (std::size_t i = 0; i < limit; ++i)
        {
            auto result = start_next_queued_turn(identity);
            if (result.error == "no_queued_input") break;
            results.push_back(result);
            if (!result.error.empty() || (result.checkpoint.phase != TurnPhase::Completed &&
                                           result.checkpoint.phase != TurnPhase::Failed))
                break;
        }
        return results;
    }
    SubscribeResult ConversationEngine::subscribe_events(const ConversationIdentity &identity,
                                                          std::uint64_t after,
                                                          std::size_t capacity)
    {
        if (!events_)
            return {{}, store_.last_event_sequence(identity), "event_stream_not_configured"};
        return events_->subscribe(identity, after, capacity);
    }
}
