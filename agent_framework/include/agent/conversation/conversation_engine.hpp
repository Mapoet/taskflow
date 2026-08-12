#pragma once
#include <functional>
#include "agent/conversation/store.hpp"
#include "agent/conversation/turn_state_machine.hpp"
#include "agent/conversation/event_stream.hpp"

namespace agent_framework::conversation
{
  using TurnExecutor = std::function<ModelTurnOutcome(const TurnRequest &, const TurnCheckpoint &)>;
  using RuntimeEventSink = std::function<void(const RuntimeEventEnvelope &)>;

  struct TurnResult
  {
    TurnCheckpoint checkpoint;
    ModelTurnOutcome outcome;
    std::string error;
  };

  class ConversationEngine
  {
  public:
    ConversationEngine(ConversationStore &store, TurnExecutor executor, RuntimeEventSink sink = {},
                       EventStreamHub *events = nullptr)
        : store_(store), executor_(std::move(executor)), sink_(std::move(sink)), events_(events) {}
    TurnResult start_turn(const TurnRequest &);
    TurnResult continue_turn(const TurnRequest &, TurnContinuationReason);
    bool interrupt_turn(const ConversationIdentity &, std::string_view, std::string * = nullptr);
    TurnResult resume_turn(const TurnRequest &, TurnContinuationReason);
    InputDisposition classify_input(std::string_view) const;
    bool submit_user_input(const TurnRequest &, InputDisposition, std::string * = nullptr);
    TurnResult start_next_queued_turn(const ConversationIdentity &);
    std::vector<TurnResult> drain_queued_turns(const ConversationIdentity &,
                                              std::size_t limit = 100);
    SubscribeResult subscribe_events(const ConversationIdentity &, std::uint64_t after = 0,
                                     std::size_t capacity = 256);

  private:
    TurnResult execute(const TurnRequest &, TurnCheckpoint);
    void event(const TurnRequest &, TurnCheckpoint &, std::string, nlohmann::json,
               EventDurability, EventVisibility);
    ConversationStore &store_;
    TurnExecutor executor_;
    RuntimeEventSink sink_;
    EventStreamHub *events_{nullptr};
  };
}
