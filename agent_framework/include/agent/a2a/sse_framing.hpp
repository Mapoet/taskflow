/**
 * @file sse_framing.hpp
 * @brief W3C Server-Sent Events 帧拼装与增量解析（A2A StreamResponse 载体，WP2.1）
 * @author Mapoet
 * @version 0.1
 * @date 2026-04-05
 */
#ifndef __AGENT_A2A_SSE_FRAMING_H__
#define __AGENT_A2A_SSE_FRAMING_H__

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agent_framework {
namespace a2a {

/**
 * @brief 单条解析后的事件（不含对 data 内 JSON 的业务解释）
 */
struct SseEvent {
    /** @brief SSE event 字段；未出现时为空串（默认 event 由上层视为 message） */
    std::string event;
    /** @brief 拼接后的 data 载荷（UTF-8） */
    std::string data;
    std::optional<std::string> id;
};

/**
 * @brief 向缓冲区追加一条 SSE 事件，以空行结束
 * @param data_payload 已序列化的 UTF-8 字符串（通常为单行 JSON）；不得含未转义的裸 \\n\\n
 */
void append_sse_event(std::string& buffer,
                      std::string_view event_name,
                      std::string_view data_payload,
                      const std::optional<std::string>& event_id = std::nullopt);

/**
 * @brief 增量解析 SSE 字节流
 */
class SseParser {
public:
    /** @brief 追加一块输入；不假设与事件边界对齐 */
    void feed(std::string_view chunk);

    /**
     * @brief 取出所有已完整的事件并从内部缓冲移除
     */
    void drain_events(std::vector<SseEvent>& out);

    /** @brief 测试/调试：当前未决缓冲大小 */
    std::size_t pending_size() const { return pending_.size(); }

private:
    std::string pending_;
};

} // namespace a2a
} // namespace agent_framework

#endif // __AGENT_A2A_SSE_FRAMING_H__
