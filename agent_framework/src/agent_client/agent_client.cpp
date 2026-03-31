/**
 * @file agent_client.cpp
 * @brief Agent 客户端实现（A2A 协议，REST JSON 与 AgentServer 对齐）
 */
#include <agent/agent_client.hpp>
#include <agent/httplib_http_client.hpp>
#include <agent/types.hpp>

#include <sstream>
#include <stdexcept>

namespace agent_framework {

namespace {

void throw_if_rest_error_body(const json& body) {
    if (!body.contains("error")) {
        return;
    }
    const auto& err = body["error"];
    if (err.is_string()) {
        throw std::runtime_error(err.get<std::string>());
    }
    if (err.is_object() && err.contains("message")) {
        throw std::runtime_error(err["message"].get<std::string>());
    }
    throw std::runtime_error("AgentClient: server returned error field in JSON body");
}

} // namespace

std::string AgentClient::join_url(const std::string& base, const std::string& path) {
    if (base.empty()) {
        return path;
    }
    if (path.empty()) {
        return base;
    }
    const bool base_slash = (base.back() == '/');
    const bool path_slash = (path.front() == '/');
    if (base_slash && path_slash) {
        return base + path.substr(1);
    }
    if (!base_slash && !path_slash) {
        return base + "/" + path;
    }
    return base + path;
}

AgentClient::AgentClient(const std::string& server_url)
    : server_url_(server_url), http_client_(std::make_unique<HttplibClient>()) {
    auth_config_ = json::object();
}

AgentClient::~AgentClient() {
    std::lock_guard<std::mutex> lock(sse_mutex_);
    sse_connections_.clear();
}

std::future<AgentCard> AgentClient::discover_agent(const std::string& agent_endpoint) {
    return std::async(std::launch::async, [this, agent_endpoint]() {
        const std::string url = join_url(server_url_, agent_endpoint);
        auto headers = build_auth_headers();
        json body = http_client_->get(url, headers);
        throw_if_rest_error_body(body);
        return AgentCard::from_json(body);
    });
}

std::future<AgentTask> AgentClient::send_task(
    const std::string& agent_endpoint,
    const AgentMessage& initial_message,
    const std::optional<std::string>& session_id,
    const json& metadata
) {
    return std::async(std::launch::async, [this, agent_endpoint, initial_message, session_id, metadata]() {
        json req_body = {
            {"message", initial_message.to_json()},
            {"metadata", metadata}
        };
        if (session_id.has_value()) {
            req_body["session_id"] = *session_id;
        }
        const std::string url = join_url(server_url_, agent_endpoint + "/tasks/send");
        auto headers = build_auth_headers();
        headers["Content-Type"] = "application/json";
        json body = http_client_->post(url, req_body, headers);
        throw_if_rest_error_body(body);
        return AgentTask::from_json(body.at("task"));
    });
}

std::future<AgentTask> AgentClient::get_task(const std::string& agent_endpoint, const std::string& task_id) {
    return std::async(std::launch::async, [this, agent_endpoint, task_id]() {
        std::ostringstream path;
        path << agent_endpoint << "/tasks/get?task_id=" << task_id;
        const std::string url = join_url(server_url_, path.str());
        auto headers = build_auth_headers();
        json body = http_client_->get(url, headers);
        throw_if_rest_error_body(body);
        return AgentTask::from_json(body.at("task"));
    });
}

std::future<bool> AgentClient::cancel_task(const std::string& agent_endpoint, const std::string& task_id) {
    return std::async(std::launch::async, [this, agent_endpoint, task_id]() {
        json req_body = {{"task_id", task_id}};
        const std::string url = join_url(server_url_, agent_endpoint + "/tasks/cancel");
        auto headers = build_auth_headers();
        headers["Content-Type"] = "application/json";
        json body = http_client_->post(url, req_body, headers);
        throw_if_rest_error_body(body);
        return body.value("success", false);
    });
}

std::future<AgentTask> AgentClient::update_task(
    const std::string& agent_endpoint,
    const std::string& task_id,
    const AgentMessage& additional_message
) {
    return std::async(std::launch::async, [this, agent_endpoint, task_id, additional_message]() {
        json req_body = {
            {"task_id", task_id},
            {"message", additional_message.to_json()}
        };
        const std::string url = join_url(server_url_, agent_endpoint + "/tasks/update");
        auto headers = build_auth_headers();
        headers["Content-Type"] = "application/json";
        json body = http_client_->post(url, req_body, headers);
        throw_if_rest_error_body(body);
        return AgentTask::from_json(body.at("task"));
    });
}

void AgentClient::subscribe_task_updates(
    const std::string& agent_endpoint,
    const std::string& task_id,
    std::function<void(const AgentTask&)> on_status_update,
    std::function<void(const AgentArtifact&)> on_artifact_update
) {
    std::lock_guard<std::mutex> lock(sse_mutex_);

    const std::string sse_key = make_sse_key(agent_endpoint, task_id);

    std::ostringstream path;
    path << agent_endpoint << "/tasks/sendSubscribe?task_id=" << task_id;
    const std::string sse_endpoint = join_url(server_url_, path.str());

    auto sse_conn = std::make_unique<SSEConnection>(sse_endpoint, task_id);
    sse_conn->subscribe(on_status_update, on_artifact_update);

    sse_connections_[sse_key] = std::move(sse_conn);
}

void AgentClient::resubscribe_task_updates(
    const std::string& agent_endpoint,
    const std::string& task_id,
    const std::string& last_event_id
) {
    std::lock_guard<std::mutex> lock(sse_mutex_);

    const std::string sse_key = make_sse_key(agent_endpoint, task_id);

    auto it = sse_connections_.find(sse_key);
    if (it != sse_connections_.end()) {
        it->second->reconnect(last_event_id);
    }
}

void AgentClient::set_push_notification(
    const std::string& agent_endpoint,
    const std::string& task_id,
    const std::string& webhook_url
) {
    json req_body = {
        {"task_id", task_id},
        {"webhook_url", webhook_url}
    };
    const std::string url = join_url(server_url_, agent_endpoint + "/tasks/pushNotification/set");
    auto headers = build_auth_headers();
    headers["Content-Type"] = "application/json";
    json body = http_client_->post(url, req_body, headers);
    throw_if_rest_error_body(body);
    (void)body;
}

std::future<json> AgentClient::get_push_notification_config(
    const std::string& agent_endpoint,
    const std::string& task_id
) {
    return std::async(std::launch::async, [this, agent_endpoint, task_id]() {
        std::ostringstream path;
        path << agent_endpoint << "/tasks/pushNotification/get?task_id=" << task_id;
        const std::string url = join_url(server_url_, path.str());
        auto headers = build_auth_headers();
        json body = http_client_->get(url, headers);
        throw_if_rest_error_body(body);
        return body;
    });
}

void AgentClient::set_authentication(const json& auth_config) {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    auth_config_ = auth_config;
}

void AgentClient::refresh_authentication() {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    // TODO: OAuth token refresh
}

std::map<std::string, std::string> AgentClient::build_auth_headers() const {
    std::lock_guard<std::mutex> lock(auth_mutex_);

    std::map<std::string, std::string> headers;

    if (!auth_config_.empty()) {
        const std::string auth_type = auth_config_.value("type", "");

        if (auth_type == "bearer") {
            const std::string token = auth_config_.value("token", "");
            headers["Authorization"] = "Bearer " + token;
        } else if (auth_type == "api_key") {
            const std::string key_name = auth_config_.value("key_name", "X-API-Key");
            const std::string key_value = auth_config_.value("key_value", "");
            headers[key_name] = key_value;
        }
    }

    return headers;
}

std::string AgentClient::make_sse_key(const std::string& agent_endpoint, const std::string& task_id) {
    return agent_endpoint + ":" + task_id;
}

} // namespace agent_framework
