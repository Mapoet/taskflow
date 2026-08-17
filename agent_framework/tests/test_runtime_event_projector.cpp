#include <cassert>
#include <filesystem>
#include "agent/ui/runtime_event_projector.hpp"

using namespace agent_framework;
int main() {
    const auto root=std::filesystem::temp_directory_path()/"agent-runtime-event-projector";
    std::filesystem::remove_all(root);std::filesystem::create_directories(root);
    conversation::SQLiteConversationStore source((root/"events.sqlite").string());
    ui::SQLiteInteractionProjectionStore target((root/"projection.sqlite").string());
    conversation::ConversationIdentity identity{"tenant","conversation"};
    auto append=[&](std::uint64_t sequence,std::string type,nlohmann::json payload,
                    conversation::EventVisibility visible=conversation::EventVisibility::User) {
        conversation::RuntimeEventEnvelope event;event.event_id="event-"+std::to_string(sequence);
        event.tenant_id=identity.tenant_id;event.conversation_id=identity.conversation_id;
        event.turn_id="turn";event.run_id="run";event.sequence=sequence;
        event.durability=conversation::EventDurability::Durable;event.visibility=visible;
        event.event_type=std::move(type);event.timestamp="now";event.payload=std::move(payload);
        assert(source.append_event(event,nullptr));
    };
    append(1,"task_semantics_decided",{{"task_id","task"},{"summary","bounded read-only"},{"raw_prompt","must not project"}});
    append(2,"decision_requested",{{"task_id","task"},{"decision_id","decision"},{"state","waiting"}});
    append(3,"tool_observation",{{"task_id","task"},{"invocation_id","tool-1"},{"summary","observed"}},conversation::EventVisibility::Operations);
    append(4,"llm_invocation_completed",{{"task_id","task"},{"invocation_id","llm-1"},
        {"summary","semantic classifier completed"},{"provider","provider"},{"model","model"},
        {"raw_prompt","must not project"}});
    ui::RuntimeEventInteractionProjector projector(source,target);
    auto first=projector.synchronize(identity);assert(first.ok&&first.applied==4&&first.runtime_head==4&&first.projection_head==4);
    auto again=projector.synchronize(identity);assert(again.ok&&again.applied==0&&again.projection_head==4);
    auto user=target.snapshot("tenant","conversation",ui::InteractionVisibility::User);
    assert(user&&user->head_sequence==4&&user->revision==4&&user->nodes.size()==3);
    for(const auto& node:user->nodes)assert(!node.display.contains("raw_prompt"));
    assert(user->nodes.back().kind==ui::InteractionNodeKind::Agent);
    assert(user->nodes.back().ref.agent_invocation_id=="llm-1");
    auto operations=target.snapshot("tenant","conversation",ui::InteractionVisibility::Operations);
    assert(operations&&operations->nodes.size()==4);

    // A second tenant/conversation sharing the same physical repositories must
    // never observe or advance the first projection cursor.
    conversation::ConversationIdentity isolated{"tenant-b","conversation-b"};
    conversation::RuntimeEventEnvelope other;other.event_id="other-1";
    other.tenant_id=isolated.tenant_id;other.conversation_id=isolated.conversation_id;
    other.turn_id="turn-b";other.run_id="run-b";other.sequence=1;
    other.durability=conversation::EventDurability::Durable;
    other.visibility=conversation::EventVisibility::User;
    other.event_type="task_semantics_decided";other.timestamp="now";
    other.payload={{"task_id","task-b"},{"summary","isolated"}};
    assert(source.append_event(other,nullptr));
    auto isolated_result=projector.synchronize(isolated);
    assert(isolated_result.ok&&isolated_result.runtime_head==1&&isolated_result.projection_head==1);
    auto isolated_snapshot=target.snapshot("tenant-b","conversation-b",ui::InteractionVisibility::User);
    assert(isolated_snapshot&&isolated_snapshot->nodes.size()==1);
    assert(isolated_snapshot->nodes.front().ref.task_id=="task-b");
    auto unchanged=target.snapshot("tenant","conversation",ui::InteractionVisibility::User);
    assert(unchanged&&unchanged->head_sequence==4&&unchanged->nodes.size()==3);
    std::filesystem::remove_all(root);
}
