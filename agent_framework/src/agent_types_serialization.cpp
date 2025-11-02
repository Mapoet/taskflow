/**
 * @file agent_types_serialization.cpp
 * @brief Agent 相关类型的序列化/反序列化实现（A2A 协议）
 * @author Mapoet
 * @version 0.1
 * @date 2025-01-XX
 */
#include <agent/types.hpp>
#include <chrono>
#include <sstream>
#include <iomanip>

namespace agent_framework {

// ============================================================================
// AgentCard 序列化/反序列化
// ============================================================================

json AgentCard::to_json() const {
    json j;
    j["name"] = name;
    j["description"] = description;
    j["provider"] = provider;
    j["api_endpoint"] = api_endpoint;
    j["capabilities"] = capabilities;
    j["authentication_scheme"] = authentication_scheme;
    
    json skills_array = json::array();
    for (const auto& skill : skills) {
        json skill_json;
        skill_json["name"] = skill.name;
        skill_json["description"] = skill.description;
        skill_json["input_schema"] = skill.input_schema;
        skill_json["output_schema"] = skill.output_schema;
        skill_json["required_capabilities"] = skill.required_capabilities;
        skills_array.push_back(skill_json);
    }
    j["skills"] = skills_array;
    
    return j;
}

AgentCard AgentCard::from_json(const json& j) {
    AgentCard card;
    card.name = j["name"].get<std::string>();
    card.description = j["description"].get<std::string>();
    card.provider = j["provider"].get<std::string>();
    card.api_endpoint = j["api_endpoint"].get<std::string>();
    card.capabilities = j["capabilities"].get<std::vector<std::string>>();
    card.authentication_scheme = j["authentication_scheme"];
    
    if (j.contains("skills") && j["skills"].is_array()) {
        for (const auto& skill_json : j["skills"]) {
            AgentSkill skill;
            skill.name = skill_json["name"].get<std::string>();
            skill.description = skill_json["description"].get<std::string>();
            skill.input_schema = skill_json["input_schema"];
            skill.output_schema = skill_json["output_schema"];
            skill.required_capabilities = skill_json["required_capabilities"].get<std::vector<std::string>>();
            card.skills.push_back(skill);
        }
    }
    
    return card;
}

// ============================================================================
// AgentPart 序列化/反序列化
// ============================================================================

json AgentPart::to_json() const {
    json j;
    
    switch (type) {
        case Type::TEXT:
            j["type"] = "text";
            if (text.has_value()) {
                j["text"] = *text;
            }
            break;
        case Type::FILE:
            j["type"] = "file";
            if (file.has_value()) {
                j["file"] = json{
                    {"mime_type", file->mime_type}
                };
                if (file->uri.has_value()) {
                    j["file"]["uri"] = *file->uri;
                }
                if (file->bytes.has_value()) {
                    // Base64 编码字节数组（实际实现可能需要 base64 库）
                    j["file"]["bytes"] = "base64:"; // TODO: 实现 base64 编码
                }
                if (file->name.has_value()) {
                    j["file"]["name"] = *file->name;
                }
            }
            break;
        case Type::DATA:
            j["type"] = "data";
            if (data.has_value()) {
                j["data"] = *data;
            }
            break;
    }
    
    return j;
}

AgentPart AgentPart::from_json(const json& j) {
    AgentPart part;
    
    std::string type_str = j["type"].get<std::string>();
    if (type_str == "text") {
        part.type = Type::TEXT;
        if (j.contains("text")) {
            part.text = j["text"].get<std::string>();
        }
    } else if (type_str == "file") {
        part.type = Type::FILE;
        if (j.contains("file")) {
            AgentFileInfo file_info;
            file_info.mime_type = j["file"]["mime_type"].get<std::string>();
            if (j["file"].contains("uri")) {
                file_info.uri = j["file"]["uri"].get<std::string>();
            }
            if (j["file"].contains("bytes")) {
                // TODO: 实现 base64 解码
                file_info.bytes = std::nullopt;
            }
            if (j["file"].contains("name")) {
                file_info.name = j["file"]["name"].get<std::string>();
            }
            part.file = file_info;
        }
    } else if (type_str == "data") {
        part.type = Type::DATA;
        if (j.contains("data")) {
            part.data = j["data"];
        }
    }
    
    return part;
}

// ============================================================================
// AgentMessage 序列化/反序列化
// ============================================================================

json AgentMessage::to_json() const {
    json j;
    
    j["role"] = (role == Role::USER) ? "user" : "agent";
    
    json parts_array = json::array();
    for (const auto& part : parts) {
        parts_array.push_back(part.to_json());
    }
    j["parts"] = parts_array;
    
    if (message_id.has_value()) {
        j["message_id"] = *message_id;
    }
    
    // 时间戳转换为 ISO 8601 字符串
    auto time_t = std::chrono::system_clock::to_time_t(timestamp);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%SZ");
    j["timestamp"] = oss.str();
    
    return j;
}

AgentMessage AgentMessage::from_json(const json& j) {
    AgentMessage msg;
    
    std::string role_str = j["role"].get<std::string>();
    msg.role = (role_str == "user") ? Role::USER : Role::AGENT;
    
    if (j.contains("parts") && j["parts"].is_array()) {
        for (const auto& part_json : j["parts"]) {
            msg.parts.push_back(AgentPart::from_json(part_json));
        }
    }
    
    if (j.contains("message_id")) {
        msg.message_id = j["message_id"].get<std::string>();
    }
    
    // 解析时间戳（ISO 8601 格式）
    if (j.contains("timestamp")) {
        std::string timestamp_str = j["timestamp"].get<std::string>();
        // TODO: 实现 ISO 8601 解析
        // 临时使用当前时间
        msg.timestamp = std::chrono::system_clock::now();
    } else {
        msg.timestamp = std::chrono::system_clock::now();
    }
    
    return msg;
}

// ============================================================================
// AgentArtifact 序列化/反序列化
// ============================================================================

json AgentArtifact::to_json() const {
    json j;
    j["artifact_id"] = artifact_id;
    j["task_id"] = task_id;
    j["metadata"] = metadata;
    j["is_immutable"] = is_immutable;
    
    json parts_array = json::array();
    for (const auto& part : parts) {
        parts_array.push_back(part.to_json());
    }
    j["parts"] = parts_array;
    
    // 时间戳
    auto time_t = std::chrono::system_clock::to_time_t(created_at);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%SZ");
    j["created_at"] = oss.str();
    
    return j;
}

AgentArtifact AgentArtifact::from_json(const json& j) {
    AgentArtifact artifact;
    artifact.artifact_id = j["artifact_id"].get<std::string>();
    artifact.task_id = j["task_id"].get<std::string>();
    artifact.metadata = j["metadata"];
    artifact.is_immutable = j.value("is_immutable", true);
    
    if (j.contains("parts") && j["parts"].is_array()) {
        for (const auto& part_json : j["parts"]) {
            artifact.parts.push_back(AgentPart::from_json(part_json));
        }
    }
    
    if (j.contains("created_at")) {
        // TODO: 实现 ISO 8601 解析
        artifact.created_at = std::chrono::system_clock::now();
    } else {
        artifact.created_at = std::chrono::system_clock::now();
    }
    
    return artifact;
}

// ============================================================================
// AgentTask 序列化/反序列化
// ============================================================================

json AgentTask::to_json() const {
    json j;
    j["task_id"] = task_id;
    if (session_id.has_value()) {
        j["session_id"] = *session_id;
    }
    
    // 状态枚举转换为字符串
    std::string status_str;
    switch (status) {
        case AgentTaskStatus::PENDING:
            status_str = "pending";
            break;
        case AgentTaskStatus::WORKING:
            status_str = "working";
            break;
        case AgentTaskStatus::COMPLETED:
            status_str = "completed";
            break;
        case AgentTaskStatus::FAILED:
            status_str = "failed";
            break;
        case AgentTaskStatus::INPUT_REQUIRED:
            status_str = "input_required";
            break;
        case AgentTaskStatus::CANCELLED:
            status_str = "cancelled";
            break;
    }
    j["status"] = status_str;
    
    // 消息列表
    json messages_array = json::array();
    for (const auto& msg : messages) {
        messages_array.push_back(msg.to_json());
    }
    j["messages"] = messages_array;
    
    // Artifacts 列表
    json artifacts_array = json::array();
    for (const auto& artifact : artifacts) {
        artifacts_array.push_back(artifact.to_json());
    }
    j["artifacts"] = artifacts_array;
    
    j["metadata"] = metadata;
    
    // 时间戳
    auto created_time_t = std::chrono::system_clock::to_time_t(created_at);
    auto updated_time_t = std::chrono::system_clock::to_time_t(updated_at);
    
    std::ostringstream oss1, oss2;
    oss1 << std::put_time(std::gmtime(&created_time_t), "%Y-%m-%dT%H:%M:%SZ");
    oss2 << std::put_time(std::gmtime(&updated_time_t), "%Y-%m-%dT%H:%M:%SZ");
    
    j["created_at"] = oss1.str();
    j["updated_at"] = oss2.str();
    
    return j;
}

AgentTask AgentTask::from_json(const json& j) {
    AgentTask task;
    task.task_id = j["task_id"].get<std::string>();
    
    if (j.contains("session_id")) {
        task.session_id = j["session_id"].get<std::string>();
    }
    
    // 解析状态
    std::string status_str = j["status"].get<std::string>();
    if (status_str == "pending") {
        task.status = AgentTaskStatus::PENDING;
    } else if (status_str == "working") {
        task.status = AgentTaskStatus::WORKING;
    } else if (status_str == "completed") {
        task.status = AgentTaskStatus::COMPLETED;
    } else if (status_str == "failed") {
        task.status = AgentTaskStatus::FAILED;
    } else if (status_str == "input_required") {
        task.status = AgentTaskStatus::INPUT_REQUIRED;
    } else if (status_str == "cancelled") {
        task.status = AgentTaskStatus::CANCELLED;
    }
    
    // 解析消息列表
    if (j.contains("messages") && j["messages"].is_array()) {
        for (const auto& msg_json : j["messages"]) {
            task.messages.push_back(AgentMessage::from_json(msg_json));
        }
    }
    
    // 解析 Artifacts 列表
    if (j.contains("artifacts") && j["artifacts"].is_array()) {
        for (const auto& artifact_json : j["artifacts"]) {
            task.artifacts.push_back(AgentArtifact::from_json(artifact_json));
        }
    }
    
    task.metadata = j["metadata"];
    
    // 解析时间戳
    // TODO: 实现 ISO 8601 解析
    task.created_at = std::chrono::system_clock::now();
    task.updated_at = std::chrono::system_clock::now();
    if (j.contains("created_at")) {
        // TODO: 解析 created_at
    }
    if (j.contains("updated_at")) {
        // TODO: 解析 updated_at
    }
    
    return task;
}

} // namespace agent_framework

