/**
 * @file agent_client.cpp
 * @brief Agent 客户端：A2A JSON-RPC（默认）与 Legacy REST（WP2.4）
 */
#include <agent/agent_client.hpp>
#include <agent/a2a/client_config.hpp>
#include <agent/a2a/jsonrpc_client.hpp>
#include <agent/a2a/wire_card.hpp>
#include <agent/a2a/wire_mapping.hpp>
#include <agent/httplib_http_client.hpp>
#include <agent/types.hpp>

#include <cstdlib>
#include <future>
#include <mutex>
#include <sstream>
#include <string>
#include <stdexcept>

namespace agent_framework {

namespace {

bool default_use_legacy_from_env() {
    const char* leg = std::getenv("AGENT_CLIENT_USE_LEGACY_REST");
    return leg != nullptr && leg[0] == '1';
}

std::string default_json_rpc_path_from_env() {
    const char* p = std::getenv("AGENT_CLIENT_JSON_RPC_PATH");
    if (p != nullptr && p[0] != '\0') {
        std::string path = p;
        if (path[0] != '/') {
            path = "/" + path;
        }
        return path;
    }
    return std::string(a2a::kA2aJsonRpcDefaultPath);
}

std::string merge_json_rpc_path(const AgentClientOptions& options) {
    if (!options.json_rpc_path.has_value()) {
        return default_json_rpc_path_from_env();
    }
    const std::string& p = *options.json_rpc_path;
    if (p.empty()) {
        return std::string(a2a::kA2aJsonRpcDefaultPath);
    }
    if (p[0] != '/') {
        return "/" + p;
    }
    return p;
}

HttplibClient& require_httplib(HTTPClient* c) {
    auto* h = dynamic_cast<HttplibClient*>(c);
    if (!h) {
        throw std::runtime_error("AgentClient: HTTP backend must be HttplibClient");
    }
    return *h;
}

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

AgentClient::AgentClient(const std::string& server_url, const AgentClientOptions& options)
    : server_url_(server_url),
      use_legacy_rest_(options.use_legacy_rest.value_or(default_use_legacy_from_env())),
      json_rpc_path_(merge_json_rpc_path(options)),
      http_client_(std::make_unique<HttplibClient>()) {
    auth_config_ = json::object();
}

AgentClient::~AgentClient() {
    std::lock_guard<std::mutex> lock(sse_mutex_);
    for (auto& kv : sse_connections_) {
        if (kv.second) {
            kv.second->close();
        }
    }
    sse_connections_.clear();
}

std::future<AgentCard> AgentClient::discover_agent(const std::string& agent_endpoint) {
    // libstdc++ may run std::launch::deferred on a thread-pool thread; httplib/OpenSSL
    // are unsafe there for our usage. Run synchronously on the caller thread and return
    // an already-ready future (see header @note).
    std::packaged_task<AgentCard()> pt([this, agent_endpoint]() {
        const std::string url = join_url(server_url_, agent_endpoint);
        auto headers = build_auth_headers();
        json body = http_client_->get(url, headers);
        throw_if_rest_error_body(body);
        if (!use_legacy_rest_) {
            return a2a::agent_card_from_a2a_wire(body);
        }
        return AgentCard::from_json(body);
    });
    std::future<AgentCard> fut = pt.get_future();
    pt();
    return fut;
}

std::future<AgentTask> AgentClient::send_task(
    const std::string& agent_endpoint,
    const AgentMessage& initial_message,
    const std::optional<std::string>& session_id,
    const json& metadata
) {
    std::packaged_task<AgentTask()> pt([this, agent_endpoint, initial_message, session_id, metadata]() {
        if (use_legacy_rest_) {
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
        }

        json meta = metadata;
        if (session_id.has_value()) {
            meta["contextId"] = *session_id;
        }
        json params = {
            {"message", a2a::message_to_a2a_wire(initial_message)},
            {"metadata", meta}
        };
        const std::string rpc_url = join_url(server_url_, json_rpc_path_);
        auto headers = build_auth_headers();
        json result = a2a::a2a_jsonrpc_post(require_httplib(http_client_.get()), rpc_url,
                                            a2a::kMethodSendMessage, params, headers,
                                            jsonrpc_next_id_);
        if (!result.contains("task")) {
            throw std::runtime_error("AgentClient: SendMessage result missing task");
        }
        return a2a::task_from_a2a_wire(result["task"]);
    });
    std::future<AgentTask> fut = pt.get_future();
    pt();
    return fut;
}

std::future<AgentTask> AgentClient::get_task(const std::string& agent_endpoint, const std::string& task_id) {
    std::packaged_task<AgentTask()> pt([this, agent_endpoint, task_id]() {
        if (use_legacy_rest_) {
            std::ostringstream path;
            path << agent_endpoint << "/tasks/get?task_id=" << task_id;
            const std::string url = join_url(server_url_, path.str());
            auto headers = build_auth_headers();
            json body = http_client_->get(url, headers);
            throw_if_rest_error_body(body);
            return AgentTask::from_json(body.at("task"));
        }

        json params = {{"id", task_id}};
        const std::string rpc_url = join_url(server_url_, json_rpc_path_);
        auto headers = build_auth_headers();
        json result = a2a::a2a_jsonrpc_post(require_httplib(http_client_.get()), rpc_url, a2a::kMethodGetTask,
                                            params, headers, jsonrpc_next_id_);
        return a2a::task_from_a2a_wire(result);
    });
    std::future<AgentTask> fut = pt.get_future();
    pt();
    return fut;
}

std::future<bool> AgentClient::cancel_task(const std::string& agent_endpoint, const std::string& task_id) {
    std::packaged_task<bool()> pt([this, agent_endpoint, task_id]() {
        if (use_legacy_rest_) {
            json req_body = {{"task_id", task_id}};
            const std::string url = join_url(server_url_, agent_endpoint + "/tasks/cancel");
            auto headers = build_auth_headers();
            headers["Content-Type"] = "application/json";
            json body = http_client_->post(url, req_body, headers);
            throw_if_rest_error_body(body);
            return body.value("success", false);
        }

        json params = {{"id", task_id}};
        const std::string rpc_url = join_url(server_url_, json_rpc_path_);
        auto headers = build_auth_headers();
        json result = a2a::a2a_jsonrpc_post(require_httplib(http_client_.get()), rpc_url,
                                            a2a::kMethodCancelTask, params, headers, jsonrpc_next_id_);
        (void)a2a::task_from_a2a_wire(result);
        return true;
    });
    std::future<bool> fut = pt.get_future();
    pt();
    return fut;
}

std::future<AgentTask> AgentClient::update_task(
    const std::string& agent_endpoint,
    const std::string& task_id,
    const AgentMessage& additional_message
) {
    std::packaged_task<AgentTask()> pt([this, agent_endpoint, task_id, additional_message]() {
        // Server 暂无 CancelTask 同级 JSON-RPC；沿用 Legacy POST（须启用 AGENT_SERVER_LEGACY_REST）
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
    std::future<AgentTask> fut = pt.get_future();
    pt();
    return fut;
}

void AgentClient::subscribe_task_updates(
    const std::string& agent_endpoint,
    const std::string& task_id,
    std::function<void(const AgentTask&)> on_status_update,
    std::function<void(const AgentArtifact&)> on_artifact_update
) {
    std::lock_guard<std::mutex> lock(sse_mutex_);

    const std::string sse_key = make_sse_key(agent_endpoint, task_id);

    std::string sse_endpoint;
    if (use_legacy_rest_) {
        std::ostringstream path;
        path << agent_endpoint << "/tasks/sendSubscribe?task_id=" << task_id;
        sse_endpoint = join_url(server_url_, path.str());
    } else {
        sse_endpoint = join_url(server_url_, std::string(a2a::kTaskSseSubscribePathQueryPrefix) + task_id);
    }

    auto headers = build_auth_headers();
    headers["Accept"] = "text/event-stream";

    auto sse_conn =
        std::make_unique<SSEConnection>(sse_endpoint, task_id, http_client_.get());
    sse_conn->subscribe(headers, std::move(on_status_update), std::move(on_artifact_update));

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
    std::packaged_task<json()> pt([this, agent_endpoint, task_id]() {
        std::ostringstream path;
        path << agent_endpoint << "/tasks/pushNotification/get?task_id=" << task_id;
        const std::string url = join_url(server_url_, path.str());
        auto headers = build_auth_headers();
        json body = http_client_->get(url, headers);
        throw_if_rest_error_body(body);
        return body;
    });
    std::future<json> fut = pt.get_future();
    pt();
    return fut;
}

void AgentClient::set_authentication(const json& auth_config) {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    auth_config_ = auth_config;
}

void AgentClient::refresh_authentication() {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    // TODO: OAuth token refresh (WP2.5)
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
