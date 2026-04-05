/**
 * @file sse_framing.cpp
 * @brief W3C SSE 帧实现（见 docs/guides/a2a-spec-tracker.md §5.2）
 */
#include <agent/a2a/sse_framing.hpp>

#include <sstream>

namespace agent_framework {
namespace a2a {
namespace {

void append_field_line(std::string& buffer, std::string_view name, std::string_view value) {
    buffer.append(name.data(), name.size());
    buffer.push_back(':');
    if (!value.empty()) {
        buffer.push_back(' ');
        buffer.append(value.data(), value.size());
    }
    buffer.push_back('\n');
}

} // namespace

void append_sse_event(std::string& buffer,
                      std::string_view event_name,
                      std::string_view data_payload,
                      const std::optional<std::string>& event_id) {
    if (!event_name.empty()) {
        append_field_line(buffer, "event", event_name);
    }
    if (event_id.has_value() && !event_id->empty()) {
        append_field_line(buffer, "id", *event_id);
    }
    // data 可能含单行 JSON；按 W3C 规则，多行 data 用多个 data: 行
    std::size_t start = 0;
    while (start < data_payload.size()) {
        std::size_t nl = data_payload.find('\n', start);
        std::string_view line = (nl == std::string_view::npos)
            ? data_payload.substr(start)
            : data_payload.substr(start, nl - start);
        append_field_line(buffer, "data", line);
        if (nl == std::string_view::npos) {
            break;
        }
        start = nl + 1;
    }
    if (data_payload.empty()) {
        append_field_line(buffer, "data", "");
    }
    buffer.push_back('\n');
}

void SseParser::feed(std::string_view chunk) {
    pending_.append(chunk.data(), chunk.size());
}

void SseParser::drain_events(std::vector<SseEvent>& out) {
    for (;;) {
        std::size_t sep = pending_.find("\r\n\r\n");
        std::size_t sep_len = 4;
        if (sep == std::string::npos) {
            sep = pending_.find("\n\n");
            sep_len = 2;
        }
        if (sep == std::string::npos) {
            return;
        }

        std::string block = pending_.substr(0, sep);
        pending_.erase(0, sep + sep_len);

        SseEvent ev;
        std::istringstream in(block);
        std::string line;
        std::string data_acc;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }
            std::size_t colon = line.find(':');
            std::string name = (colon == std::string::npos) ? line : line.substr(0, colon);
            std::string value = (colon == std::string::npos) ? std::string() : line.substr(colon + 1);
            if (!value.empty() && value[0] == ' ') {
                value.erase(0, 1);
            }
            if (name == "event") {
                ev.event = value;
            } else if (name == "data") {
                if (!data_acc.empty()) {
                    data_acc.push_back('\n');
                }
                data_acc += value;
            } else if (name == "id") {
                ev.id = value;
            }
        }
        ev.data = std::move(data_acc);
        out.push_back(std::move(ev));
    }
}

} // namespace a2a
} // namespace agent_framework
