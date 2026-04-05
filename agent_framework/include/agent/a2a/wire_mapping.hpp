/**
 * @file wire_mapping.hpp
 * @brief A2A v1 ProtoJSON ↔ agent_framework::AgentTask / Message / Part / Artifact（WP2.1）
 * @author Mapoet
 * @version 0.1
 * @date 2026-04-05
 */
#ifndef __AGENT_A2A_WIRE_MAPPING_H__
#define __AGENT_A2A_WIRE_MAPPING_H__

#include <agent/a2a/sse_framing.hpp>
#include <agent/types.hpp>

#include <optional>
#include <string>

namespace agent_framework {
namespace a2a {

/** @brief Task 的 ProtoJSON（camelCase，见 a2a-spec-tracker.md §6） */
json task_to_a2a_wire(const AgentTask& task);
AgentTask task_from_a2a_wire(const json& wire);

json message_to_a2a_wire(const AgentMessage& message);
AgentMessage message_from_a2a_wire(const json& wire);

json part_to_a2a_wire(const AgentPart& part);
AgentPart part_from_a2a_wire(const json& wire);

json artifact_to_a2a_wire(const AgentArtifact& artifact);
AgentArtifact artifact_from_a2a_wire(const json& wire);

std::string agent_task_status_to_a2a_state(AgentTaskStatus s);
AgentTaskStatus agent_task_status_from_a2a_state(std::string_view state);

/**
 * @brief 从 SSE 事件的 data（JSON）中解析 statusUpdate
 * @return 若根对象含 `statusUpdate` 且可解析 taskId/state 则 true，并写入 out（仅填充 task_id、status、updated_at）
 */
bool try_parse_task_status_sse(const SseEvent& event, AgentTask& out);

/** @brief StreamResponse.statusUpdate 根对象（ProtoJSON） */
json stream_response_status_update(const AgentTask& task);

/** @brief StreamResponse.artifactUpdate 根对象（ProtoJSON） */
json stream_response_artifact_update(const AgentArtifact& artifact,
                                     const std::string& task_id,
                                     const std::optional<std::string>& context_id);

} // namespace a2a
} // namespace agent_framework

#endif // __AGENT_A2A_WIRE_MAPPING_H__
