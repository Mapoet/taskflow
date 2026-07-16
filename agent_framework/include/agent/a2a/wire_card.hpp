/**
 * @file wire_card.hpp
 * @brief Agent Card ↔ A2A 官方 Well-Known JSON 映射（WP2.1a）
 * @author Mapoet
 * @version 0.1
 * @date 2026-04-05
 */
#ifndef __AGENT_A2A_WIRE_CARD_H__
#define __AGENT_A2A_WIRE_CARD_H__

#include <agent/core/types.hpp>

#include <string>

namespace agent_framework {
namespace a2a {

/**
 * @brief 将 `AgentCard` 序列化为 A2A 官方 Agent Card JSON（camelCase，见 a2a-spec-tracker.md §4）
 */
::json agent_card_to_a2a_wire(const AgentCard& card);

/**
 * @brief 自 Well-Known / 官方 JSON 反序列化
 * @throws std::invalid_argument 缺 tracker §4 标明之必填键或类型不合法
 */
AgentCard agent_card_from_a2a_wire(const ::json& j);

/**
 * @brief UTF-8、无 BOM 的发现用 JSON 字符串（紧凑 dump）
 */
std::string agent_card_discovery_json_string(const AgentCard& card);

} // namespace a2a
} // namespace agent_framework

#endif // __AGENT_A2A_WIRE_CARD_H__
