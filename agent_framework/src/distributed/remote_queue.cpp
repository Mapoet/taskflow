#include "agent/distributed/remote_queue.hpp"

#include <httplib.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

namespace agent_framework::distributed
{
    namespace
    {
        using json = nlohmann::json;
        constexpr std::size_t kMaximumRequestBytes = 64 * 1024;
        std::int64_t wall_time_ms()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        }

        json task_json(const QueueTask &v)
        {
            return {{"task_id", v.task_id}, {"tenant_id", v.tenant_id}, {"idempotency_key", v.idempotency_key}, {"payload_digest", v.payload_digest}, {"priority", v.priority}, {"available_at_ms", v.available_at_ms}, {"attempts", v.attempts}, {"max_attempts", v.max_attempts}, {"state", static_cast<int>(v.state)}, {"owner", v.owner}, {"fencing_token", v.fencing_token}, {"lease_expires_at_ms", v.lease_expires_at_ms}};
        }

        QueueTask parse_task(const json &v)
        {
            QueueTask result;
            result.task_id = v.at("task_id").get<std::string>();
            result.tenant_id = v.at("tenant_id").get<std::string>();
            result.idempotency_key = v.at("idempotency_key").get<std::string>();
            result.payload_digest = v.at("payload_digest").get<std::string>();
            result.priority = v.value("priority", 0);
            result.available_at_ms = v.value("available_at_ms", std::int64_t{0});
            result.attempts = v.value("attempts", std::uint32_t{0});
            result.max_attempts = v.value("max_attempts", std::uint32_t{3});
            const int state = v.value("state", 0);
            if (state < 0 || state > static_cast<int>(QueueState::DeadLetter))
                throw std::invalid_argument("invalid queue state");
            result.state = static_cast<QueueState>(state);
            result.owner = v.value("owner", std::string{});
            result.fencing_token = v.value("fencing_token", std::uint64_t{0});
            result.lease_expires_at_ms = v.value("lease_expires_at_ms", std::int64_t{0});
            return result;
        }

        void response(httplib::Response &out, int status, json body)
        {
            out.status = status;
            out.set_content(body.dump(), "application/json");
        }

        bool authorized(const httplib::Request &request, std::string_view token)
        {
            const auto supplied = request.get_header_value("Authorization");
            const auto expected = std::string("Bearer ") + std::string(token);
            if (token.empty() || supplied.size() != expected.size())
                return false;
            unsigned char difference = 0;
            for (std::size_t i = 0; i < expected.size(); ++i)
                difference |= static_cast<unsigned char>(supplied[i] ^ expected[i]);
            return difference == 0;
        }

        template <typename Action>
        void endpoint(const httplib::Request &request, httplib::Response &out,
                      std::string_view token, Action &&action)
        {
            if (!authorized(request, token))
                return response(out, 401, {{"error", "unauthorized"}});
            if (request.body.size() > kMaximumRequestBytes)
                return response(out, 413, {{"error", "request_too_large"}});
            try
            {
                action(json::parse(request.body), out);
            }
            catch (const std::exception &error)
            {
                response(out, 400, {{"error", error.what()}});
            }
        }

    } // namespace

    class RemoteQueueServer::Impl
    {
    public:
        Impl(SQLiteDurableQueue &queue, std::string token,
             std::optional<RemoteQueueTlsServerConfig> tls)
            : queue_(queue), token_(std::move(token)), tls_(tls.has_value())
        {
            if (token_.empty())
                throw std::invalid_argument("remote queue bearer token required");
            if (tls)
            {
                if (tls->certificate_path.empty() || tls->private_key_path.empty() ||
                    tls->client_ca_path.empty())
                    throw std::invalid_argument("mTLS certificate, key, and client CA are required");
                auto secure = std::make_unique<httplib::SSLServer>(
                    tls->certificate_path.c_str(), tls->private_key_path.c_str(),
                    tls->client_ca_path.c_str());
                if (!secure->is_valid())
                    throw std::runtime_error("invalid remote queue mTLS server configuration");
                if (!secure->set_min_tls_version(TLS1_2_VERSION))
                    throw std::runtime_error("cannot enforce TLS 1.2 minimum");
                server_ = std::move(secure);
            }
            else
                server_ = std::make_unique<httplib::Server>();
            server_->Post("/v1/queue/enqueue", [this](const auto &req, auto &out)
                         { endpoint(req, out, token_, [this](const json &input, auto &result)
                                    {
                std::string error;
                auto task = parse_task(input.at("task"));
                if(task.state != QueueState::Pending || task.attempts != 0 ||
                   !task.owner.empty() || task.fencing_token != 0 || task.lease_expires_at_ms != 0)
                    throw std::invalid_argument("client cannot set queue operational state");
                const bool ok = queue_.enqueue(std::move(task), &error);
                response(result, ok ? 200 : 409, {{"ok", ok}, {"error", error}}); }); });
            server_->Post("/v1/queue/claim", [this](const auto &req, auto &out)
                         { endpoint(req, out, token_, [this](const json &input, auto &result)
                                    {
                auto lease = queue_.claim_with_quota(input.at("worker_id").get<std::string>(),
                    input.at("tenant_id").get<std::string>(), wall_time_ms(),
                    input.at("lease_ms").get<std::int64_t>());
                if(!lease) return response(result, 200, {{"ok", true}, {"lease", nullptr}});
                response(result, 200, {{"ok", true}, {"lease", {{"task", task_json(lease->task)},
                    {"fencing_token", lease->fencing_token}}}}); }); });
            server_->Post("/v1/queue/renew", [this](const auto &req, auto &out)
                         { endpoint(req, out, token_, [this](const json &v, auto &result)
                                    {
                const bool ok = queue_.renew(v.at("task_id").get<std::string>(),
                    v.at("worker_id").get<std::string>(), v.at("fencing_token").get<std::uint64_t>(),
                    wall_time_ms(), v.at("lease_ms").get<std::int64_t>());
                response(result, ok ? 200 : 409, {{"ok", ok}}); }); });
            server_->Post("/v1/queue/ack", [this](const auto &req, auto &out)
                         { endpoint(req, out, token_, [this](const json &v, auto &result)
                                    {
                const bool ok = queue_.ack_with_quota_at(v.at("task_id").get<std::string>(),
                    v.at("worker_id").get<std::string>(), v.at("fencing_token").get<std::uint64_t>(),
                    wall_time_ms());
                response(result, ok ? 200 : 409, {{"ok", ok}}); }); });
            server_->Post("/v1/queue/nack", [this](const auto &req, auto &out)
                         { endpoint(req, out, token_, [this](const json &v, auto &result)
                                    {
                const auto now = wall_time_ms();
                const auto delay = v.at("retry_delay_ms").get<std::int64_t>();
                const bool valid = delay >= 0 && now <= std::numeric_limits<std::int64_t>::max() - delay;
                const bool ok = valid && queue_.nack_with_quota_at(v.at("task_id").get<std::string>(),
                    v.at("worker_id").get<std::string>(), v.at("fencing_token").get<std::uint64_t>(),
                    now, now + delay);
                response(result, ok ? 200 : 409, {{"ok", ok}}); }); });
            server_->Post("/v1/queue/inspect", [this](const auto &req, auto &out)
                         { endpoint(req, out, token_, [this](const json &v, auto &result)
                                    {
                auto task = queue_.inspect(v.at("task_id").get<std::string>());
                response(result, 200, {{"ok", true}, {"task", task ? task_json(*task) : json(nullptr)}}); }); });
        }
        SQLiteDurableQueue &queue_;
        std::string token_;
        bool tls_{false};
        std::unique_ptr<httplib::Server> server_;
    };

    RemoteQueueServer::RemoteQueueServer(SQLiteDurableQueue &queue, std::string token)
        : impl_(std::make_unique<Impl>(queue, std::move(token), std::nullopt)) {}
    RemoteQueueServer::RemoteQueueServer(SQLiteDurableQueue &queue, std::string token,
                                         RemoteQueueTlsServerConfig tls)
        : impl_(std::make_unique<Impl>(queue, std::move(token), std::move(tls))) {}
    RemoteQueueServer::~RemoteQueueServer() = default;
    int RemoteQueueServer::bind(std::string_view host, int port)
    {
        // Plain HTTP is intentionally loopback-only. A cross-host deployment must
        // terminate mutually authenticated TLS in a trusted sidecar/proxy.
        if (!impl_->tls_ && host != "127.0.0.1" && host != "localhost" && host != "::1")
            return -1;
        const std::string address(host);
        return port == 0 ? impl_->server_->bind_to_any_port(address.c_str())
                         : (impl_->server_->bind_to_port(address.c_str(), port) ? port : -1);
    }
    bool RemoteQueueServer::listen_after_bind() { return impl_->server_->listen_after_bind(); }
    void RemoteQueueServer::stop() { impl_->server_->stop(); }

    RemoteQueueClient::RemoteQueueClient(std::string host, int port, std::string token, int timeout)
        : host_(std::move(host)), port_(port), bearer_token_(std::move(token)), timeout_seconds_(timeout)
    {
        if (host_.empty() || port_ <= 0 || port_ > 65535 || bearer_token_.empty() || timeout_seconds_ <= 0)
            throw std::invalid_argument("invalid remote queue client configuration");
    }
    RemoteQueueClient::RemoteQueueClient(std::string host, int port, std::string token,
                                         RemoteQueueTlsClientConfig tls, int timeout)
        : host_(std::move(host)), port_(port), bearer_token_(std::move(token)),
          timeout_seconds_(timeout), tls_(std::move(tls))
    {
        if (host_.empty() || port_ <= 0 || port_ > 65535 || bearer_token_.empty() ||
            timeout_seconds_ <= 0 || tls_->ca_path.empty() ||
            tls_->certificate_path.empty() || tls_->private_key_path.empty())
            throw std::invalid_argument("invalid remote queue mTLS client configuration");
    }

    namespace
    {
        std::optional<json> request(std::string_view host, int port, std::string_view token,
                                    int timeout, std::string_view path, const json &body,
                                    const std::optional<RemoteQueueTlsClientConfig> &tls,
                                    std::string *error)
        {
            if (error)
                error->clear();
            std::unique_ptr<httplib::Client> client;
            if (tls)
            {
                const auto endpoint = std::string("https://") + std::string(host) + ":" +
                                      std::to_string(port);
                client = std::make_unique<httplib::Client>(endpoint.c_str(),
                    tls->certificate_path, tls->private_key_path);
                client->set_ca_cert_path(tls->ca_path.c_str());
                client->enable_server_certificate_verification(true);
                if (!client->ssl_context() ||
                    SSL_CTX_set_min_proto_version(client->ssl_context(), TLS1_2_VERSION) != 1)
                {
                    if (error)
                        *error = "cannot enforce TLS 1.2 minimum";
                    return std::nullopt;
                }
            }
            else
                client = std::make_unique<httplib::Client>(std::string(host), port);
            client->set_connection_timeout(timeout, 0);
            client->set_read_timeout(timeout, 0);
            client->set_write_timeout(timeout, 0);
            httplib::Headers headers{{"Authorization", std::string("Bearer ") + std::string(token)}};
            const std::string endpoint(path);
            auto result = client->Post(endpoint.c_str(), headers, body.dump(), "application/json");
            if (!result)
            {
                if (error)
                    *error = "remote queue transport unavailable";
                return std::nullopt;
            }
            try
            {
                auto value = json::parse(result->body);
                if (result->status != 200 || !value.value("ok", false))
                {
                    if (error)
                        *error = value.value("error", "remote queue operation rejected");
                    return std::nullopt;
                }
                return value;
            }
            catch (const std::exception &x)
            {
                if (error)
                    *error = x.what();
                return std::nullopt;
            }
        }
    } // namespace

    bool RemoteQueueClient::enqueue(QueueTask task, std::string *error) const
    {
        return request(host_, port_, bearer_token_, timeout_seconds_, "/v1/queue/enqueue",
                       {{"task", task_json(task)}}, tls_, error)
            .has_value();
    }
    std::optional<Lease> RemoteQueueClient::claim(std::string_view worker, std::string_view tenant,
                                                  std::int64_t lease_ms, std::string *error) const
    {
        auto value = request(host_, port_, bearer_token_, timeout_seconds_, "/v1/queue/claim",
                             {{"worker_id", worker}, {"tenant_id", tenant}, {"lease_ms", lease_ms}}, tls_, error);
        if (!value || value->at("lease").is_null())
            return std::nullopt;
        const auto &lease = value->at("lease");
        return Lease{parse_task(lease.at("task")), lease.at("fencing_token").get<std::uint64_t>()};
    }
    bool RemoteQueueClient::renew(std::string_view id, std::string_view worker, std::uint64_t token,
                                  std::int64_t lease_ms, std::string *error) const
    {
        return request(host_, port_, bearer_token_, timeout_seconds_, "/v1/queue/renew",
                       {{"task_id", id}, {"worker_id", worker}, {"fencing_token", token}, {"lease_ms", lease_ms}}, tls_, error)
            .has_value();
    }
    bool RemoteQueueClient::ack(std::string_view id, std::string_view worker, std::uint64_t token,
                                std::string *error) const
    {
        return request(host_, port_, bearer_token_, timeout_seconds_, "/v1/queue/ack",
                       {{"task_id", id}, {"worker_id", worker}, {"fencing_token", token}}, tls_, error)
            .has_value();
    }
    bool RemoteQueueClient::nack(std::string_view id, std::string_view worker, std::uint64_t token,
                                 std::int64_t retry_delay, std::string *error) const
    {
        return request(host_, port_, bearer_token_, timeout_seconds_, "/v1/queue/nack",
                       {{"task_id", id}, {"worker_id", worker}, {"fencing_token", token}, {"retry_delay_ms", retry_delay}}, tls_, error)
            .has_value();
    }
    std::optional<QueueTask> RemoteQueueClient::inspect(std::string_view id, std::string *error) const
    {
        auto value = request(host_, port_, bearer_token_, timeout_seconds_, "/v1/queue/inspect",
                             {{"task_id", id}}, tls_, error);
        if (!value || value->at("task").is_null())
            return std::nullopt;
        return parse_task(value->at("task"));
    }

} // namespace agent_framework::distributed
