/**
 * @file wire_mapping.cpp
 * @brief ProtoJSON 映射（见 docs/guides/a2a-spec-tracker.md §6）
 */
#include <agent/a2a/wire_mapping.hpp>

#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace agent_framework {
namespace a2a {
namespace {

std::string time_point_to_iso8601_utc_ms(std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    auto ms = duration_cast<milliseconds>(tp.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(tp);
    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(4) << (tm_buf.tm_year + 1900) << '-'
        << std::setw(2) << (tm_buf.tm_mon + 1) << '-' << std::setw(2) << tm_buf.tm_mday << 'T'
        << std::setw(2) << tm_buf.tm_hour << ':' << std::setw(2) << tm_buf.tm_min << ':'
        << std::setw(2) << tm_buf.tm_sec << '.' << std::setw(3) << ms.count() << 'Z';
    return oss.str();
}

std::chrono::system_clock::time_point parse_iso8601_utc_loose(std::string_view s) {
    // 最小子集：YYYY-MM-DDTHH:MM:SS[.sss]Z
    std::string str(s);
    std::tm tm{};
    int year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0;
    int ms = 0;
    char tz = 0;
    int n = std::sscanf(str.c_str(), "%d-%d-%dT%d:%d:%d.%d%c", &year, &mon, &day, &hour, &min, &sec,
                        &ms, &tz);
    if (n < 6) {
        n = std::sscanf(str.c_str(), "%d-%d-%dT%d:%d:%d%c", &year, &mon, &day, &hour, &min, &sec,
                        &tz);
        ms = 0;
    }
    if (n < 6) {
        return std::chrono::system_clock::time_point{};
    }
    tm.tm_year = year - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;
#if defined(_WIN32)
    std::time_t tt = _mkgmtime(&tm);
#else
    std::time_t tt = timegm(&tm);
#endif
    if (tt == static_cast<std::time_t>(-1)) {
        return std::chrono::system_clock::time_point{};
    }
    auto base = std::chrono::system_clock::from_time_t(tt);
    return base + std::chrono::milliseconds(ms);
}

json task_status_to_wire(AgentTaskStatus st, std::chrono::system_clock::time_point ts) {
    json j;
    j["state"] = agent_task_status_to_a2a_state(st);
    j["timestamp"] = time_point_to_iso8601_utc_ms(ts);
    return j;
}

} // namespace

std::string agent_task_status_to_a2a_state(AgentTaskStatus s) {
    switch (s) {
    case AgentTaskStatus::PENDING:
        return "TASK_STATE_SUBMITTED";
    case AgentTaskStatus::WORKING:
        return "TASK_STATE_WORKING";
    case AgentTaskStatus::COMPLETED:
        return "TASK_STATE_COMPLETED";
    case AgentTaskStatus::FAILED:
        return "TASK_STATE_FAILED";
    case AgentTaskStatus::INPUT_REQUIRED:
        return "TASK_STATE_INPUT_REQUIRED";
    case AgentTaskStatus::CANCELLED:
        return "TASK_STATE_CANCELED";
    }
    return "TASK_STATE_UNSPECIFIED";
}

AgentTaskStatus agent_task_status_from_a2a_state(std::string_view state) {
    if (state == "TASK_STATE_SUBMITTED") {
        return AgentTaskStatus::PENDING;
    }
    if (state == "TASK_STATE_WORKING") {
        return AgentTaskStatus::WORKING;
    }
    if (state == "TASK_STATE_COMPLETED") {
        return AgentTaskStatus::COMPLETED;
    }
    if (state == "TASK_STATE_FAILED") {
        return AgentTaskStatus::FAILED;
    }
    if (state == "TASK_STATE_CANCELED") {
        return AgentTaskStatus::CANCELLED;
    }
    if (state == "TASK_STATE_INPUT_REQUIRED") {
        return AgentTaskStatus::INPUT_REQUIRED;
    }
    if (state == "TASK_STATE_REJECTED") {
        return AgentTaskStatus::FAILED;
    }
    if (state == "TASK_STATE_AUTH_REQUIRED") {
        return AgentTaskStatus::INPUT_REQUIRED;
    }
    return AgentTaskStatus::FAILED;
}

json part_to_a2a_wire(const AgentPart& part) {
    json j;
    switch (part.type) {
    case AgentPart::Type::TEXT:
        if (part.text.has_value()) {
            j["text"] = *part.text;
        }
        j["mediaType"] = "text/plain";
        break;
    case AgentPart::Type::FILE:
        if (part.file.has_value()) {
            if (part.file->uri.has_value()) {
                j["url"] = *part.file->uri;
            }
            j["mediaType"] = part.file->mime_type.empty() ? "application/octet-stream" : part.file->mime_type;
            if (part.file->name.has_value()) {
                j["filename"] = *part.file->name;
            }
        }
        break;
    case AgentPart::Type::DATA:
        if (part.data.has_value()) {
            j["data"] = *part.data;
        }
        j["mediaType"] = "application/json";
        break;
    }
    return j;
}

AgentPart part_from_a2a_wire(const json& wire) {
    AgentPart part;
    if (wire.contains("text") && wire["text"].is_string()) {
        part.type = AgentPart::Type::TEXT;
        part.text = wire["text"].get<std::string>();
        return part;
    }
    if (wire.contains("url") && wire["url"].is_string()) {
        part.type = AgentPart::Type::FILE;
        AgentFileInfo fi;
        fi.uri = wire["url"].get<std::string>();
        fi.mime_type = wire.value("mediaType", "application/octet-stream");
        if (wire.contains("filename")) {
            fi.name = wire["filename"].get<std::string>();
        }
        part.file = std::move(fi);
        return part;
    }
    if (wire.contains("raw")) {
        part.type = AgentPart::Type::FILE;
        AgentFileInfo fi;
        fi.mime_type = wire.value("mediaType", "application/octet-stream");
        if (wire.contains("filename")) {
            fi.name = wire["filename"].get<std::string>();
        }
        part.file = std::move(fi);
        return part;
    }
    if (wire.contains("data")) {
        part.type = AgentPart::Type::DATA;
        part.data = wire["data"];
        return part;
    }
    part.type = AgentPart::Type::TEXT;
    part.text = std::string();
    return part;
}

json message_to_a2a_wire(const AgentMessage& message) {
    json j;
    j["messageId"] = message.message_id.value_or("_");
    j["role"] = (message.role == AgentMessage::Role::USER) ? "ROLE_USER" : "ROLE_AGENT";
    json parts = json::array();
    for (const auto& p : message.parts) {
        parts.push_back(part_to_a2a_wire(p));
    }
    j["parts"] = std::move(parts);
    return j;
}

AgentMessage message_from_a2a_wire(const json& wire) {
    AgentMessage msg;
    if (!wire.contains("messageId") || !wire["messageId"].is_string()) {
        throw std::invalid_argument("message_from_a2a_wire: missing messageId");
    }
    msg.message_id = wire["messageId"].get<std::string>();
    std::string role = wire.value("role", std::string("ROLE_USER"));
    msg.role = (role == "ROLE_AGENT") ? AgentMessage::Role::AGENT : AgentMessage::Role::USER;
    if (!wire.contains("parts") || !wire["parts"].is_array()) {
        throw std::invalid_argument("message_from_a2a_wire: missing parts array");
    }
    for (const auto& pj : wire["parts"]) {
        msg.parts.push_back(part_from_a2a_wire(pj));
    }
    msg.timestamp = std::chrono::system_clock::now();
    return msg;
}

json artifact_to_a2a_wire(const AgentArtifact& artifact) {
    json j;
    j["artifactId"] = artifact.artifact_id;
    json parts = json::array();
    for (const auto& p : artifact.parts) {
        parts.push_back(part_to_a2a_wire(p));
    }
    j["parts"] = std::move(parts);
    j["metadata"] = artifact.metadata;
    return j;
}

AgentArtifact artifact_from_a2a_wire(const json& wire) {
    AgentArtifact a;
    if (!wire.contains("artifactId")) {
        throw std::invalid_argument("artifact_from_a2a_wire: missing artifactId");
    }
    a.artifact_id = wire["artifactId"].get<std::string>();
    a.task_id.clear();
    a.metadata = wire.value("metadata", json::object());
    if (wire.contains("parts") && wire["parts"].is_array()) {
        for (const auto& pj : wire["parts"]) {
            a.parts.push_back(part_from_a2a_wire(pj));
        }
    }
    a.created_at = std::chrono::system_clock::now();
    return a;
}

json task_to_a2a_wire(const AgentTask& task) {
    json j;
    j["id"] = task.task_id;
    if (task.session_id.has_value()) {
        j["contextId"] = *task.session_id;
    }
    j["status"] = task_status_to_wire(task.status, task.updated_at);
    json hist = json::array();
    for (const auto& m : task.messages) {
        hist.push_back(message_to_a2a_wire(m));
    }
    j["history"] = std::move(hist);
    json arts = json::array();
    for (const auto& a : task.artifacts) {
        arts.push_back(artifact_to_a2a_wire(a));
    }
    j["artifacts"] = std::move(arts);
    j["metadata"] = task.metadata;
    return j;
}

AgentTask task_from_a2a_wire(const json& wire) {
    AgentTask task;
    if (!wire.contains("id") || !wire["id"].is_string()) {
        throw std::invalid_argument("task_from_a2a_wire: missing id");
    }
    task.task_id = wire["id"].get<std::string>();
    if (wire.contains("contextId") && wire["contextId"].is_string()) {
        task.session_id = wire["contextId"].get<std::string>();
    }
    if (!wire.contains("status") || !wire["status"].is_object()) {
        throw std::invalid_argument("task_from_a2a_wire: missing status");
    }
    const json& st = wire["status"];
    std::string state = st.value("state", std::string());
    task.status = agent_task_status_from_a2a_state(state);
    if (st.contains("timestamp")) {
        task.updated_at = parse_iso8601_utc_loose(st["timestamp"].get<std::string>());
    } else {
        task.updated_at = std::chrono::system_clock::now();
    }
    task.created_at = std::chrono::system_clock::time_point{};
    if (wire.contains("history") && wire["history"].is_array()) {
        for (const auto& mj : wire["history"]) {
            task.messages.push_back(message_from_a2a_wire(mj));
        }
    }
    if (wire.contains("artifacts") && wire["artifacts"].is_array()) {
        for (const auto& aj : wire["artifacts"]) {
            task.artifacts.push_back(artifact_from_a2a_wire(aj));
        }
    }
    task.metadata = wire.value("metadata", json::object());
    return task;
}

bool try_parse_task_status_sse(const SseEvent& event, AgentTask& out) {
    json root;
    try {
        root = json::parse(event.data);
    } catch (...) {
        return false;
    }
    if (!root.contains("statusUpdate") || !root["statusUpdate"].is_object()) {
        return false;
    }
    const json& su = root["statusUpdate"];
    if (!su.contains("taskId") || !su["taskId"].is_string()) {
        return false;
    }
    if (!su.contains("status") || !su["status"].is_object()) {
        return false;
    }
    const json& st = su["status"];
    if (!st.contains("state")) {
        return false;
    }
    out = AgentTask{};
    out.task_id = su["taskId"].get<std::string>();
    out.status = agent_task_status_from_a2a_state(st["state"].get<std::string>());
    if (st.contains("timestamp")) {
        out.updated_at = parse_iso8601_utc_loose(st["timestamp"].get<std::string>());
    } else {
        out.updated_at = std::chrono::system_clock::now();
    }
    return true;
}

bool try_parse_task_message_sse(const SseEvent& event, AgentTask& out) {
    json root;
    try { root = json::parse(event.data); } catch(...) { return false; }
    if(!root.contains("message") || !root["message"].is_object()) return false;
    try {
        out = AgentTask{};
        if(root.contains("metadata") && root["metadata"].is_object()) {
            out.task_id = root["metadata"].value("taskId", std::string{});
            if(root["metadata"].contains("contextId") && root["metadata"]["contextId"].is_string())
                out.session_id = root["metadata"]["contextId"].get<std::string>();
        }
        if(out.task_id.empty()) return false;
        out.status = AgentTaskStatus::WORKING;
        out.updated_at = std::chrono::system_clock::now();
        out.messages.push_back(message_from_a2a_wire(root["message"]));
        out.metadata = root["message"].value("metadata", json::object());
        return true;
    } catch(...) { return false; }
}

json stream_response_status_update(const AgentTask& task) {
    json su;
    su["taskId"] = task.task_id;
    if (task.session_id.has_value()) {
        su["contextId"] = *task.session_id;
    }
    su["status"] = task_status_to_wire(task.status, task.updated_at);
    json root;
    root["statusUpdate"] = std::move(su);
    return root;
}

json stream_response_message_delta(const std::string& task_id,
                                   const std::optional<std::string>& context_id,
                                   std::string_view message_id,
                                   std::string_view text,
                                   std::string_view channel) {
    json message;
    message["messageId"] = message_id;
    message["role"] = "ROLE_AGENT";
    message["parts"] = json::array({json{{"type", "text"}, {"text", text}}});
    message["metadata"] = {{"streamChannel", channel}, {"append", true}};
    json root;
    root["message"] = std::move(message);
    root["metadata"] = {{"taskId", task_id}};
    if(context_id) root["metadata"]["contextId"] = *context_id;
    return root;
}

json stream_response_artifact_update(const AgentArtifact& artifact,
                                     const std::string& task_id,
                                     const std::optional<std::string>& context_id) {
    json au;
    au["taskId"] = task_id;
    if (context_id.has_value()) {
        au["contextId"] = *context_id;
    }
    au["artifact"] = artifact_to_a2a_wire(artifact);
    au["append"] = false;
    au["lastChunk"] = true;
    json root;
    root["artifactUpdate"] = std::move(au);
    return root;
}

} // namespace a2a
} // namespace agent_framework
