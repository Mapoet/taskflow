#include "agent/distributed/remote_object_store.hpp"

#include <httplib.hpp>
#include <nlohmann/json.hpp>

#include <stdexcept>
#include <utility>

namespace agent_framework::distributed
{
    namespace
    {
        using json = nlohmann::json;
        constexpr std::size_t kMaximumPayload = 16 * 1024 * 1024;

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
        void respond(httplib::Response &out, int status, json body)
        {
            out.status = status;
            out.set_content(body.dump(), "application/json");
        }
        json ref_json(const ObjectRef &v) { return {{"tenant_id", v.tenant_id}, {"digest", v.digest}, {"size", v.size}, {"media_type", v.media_type}}; }
        ObjectRef parse_ref(const json &v) { return {v.at("tenant_id").get<std::string>(), v.at("digest").get<std::string>(), v.at("size").get<std::size_t>(), v.at("media_type").get<std::string>()}; }

        std::unique_ptr<httplib::Client> make_client(std::string_view host, int port,
                                                     const std::optional<RemoteQueueTlsClientConfig> &tls, std::string *error)
        {
            std::unique_ptr<httplib::Client> client;
            if (tls)
            {
                const auto endpoint = std::string("https://") + std::string(host) + ":" + std::to_string(port);
                client = std::make_unique<httplib::Client>(endpoint.c_str(), tls->certificate_path, tls->private_key_path);
                client->set_ca_cert_path(tls->ca_path.c_str());
                client->enable_server_certificate_verification(true);
                if (!client->ssl_context() || SSL_CTX_set_min_proto_version(client->ssl_context(), TLS1_2_VERSION) != 1)
                {
                    if (error)
                        *error = "cannot enforce TLS 1.2 minimum";
                    return {};
                }
            }
            else
                client = std::make_unique<httplib::Client>(std::string(host), port);
            return client;
        }
        std::optional<json> request(std::string_view host, int port, std::string_view token, int timeout,
                                    std::string_view path, const json &body, const std::optional<RemoteQueueTlsClientConfig> &tls, std::string *error)
        {
            if (error)
                error->clear();
            auto client = make_client(host, port, tls, error);
            if (!client)
                return std::nullopt;
            client->set_connection_timeout(timeout, 0);
            client->set_read_timeout(timeout, 0);
            client->set_write_timeout(timeout, 0);
            httplib::Headers headers{{"Authorization", std::string("Bearer ") + std::string(token)}};
            const std::string endpoint(path);
            auto result = client->Post(endpoint.c_str(), headers, body.dump(), "application/json");
            if (!result)
            {
                if (error)
                    *error = "remote object transport unavailable";
                return std::nullopt;
            }
            try
            {
                auto value = json::parse(result->body);
                if (result->status != 200 || !value.value("ok", false))
                {
                    if (error)
                        *error = value.value("error", "remote object operation rejected");
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

    class RemoteObjectStoreServer::Impl
    {
    public:
        Impl(ObjectStore &store, std::string token, std::optional<RemoteQueueTlsServerConfig> tls) : store_(store), token_(std::move(token)), tls_(tls.has_value())
        {
            if (token_.empty())
                throw std::invalid_argument("remote object bearer token required");
            if (tls)
            {
                if (tls->certificate_path.empty() || tls->private_key_path.empty() || tls->client_ca_path.empty())
                    throw std::invalid_argument("mTLS certificate, key, and client CA are required");
                auto secure = std::make_unique<httplib::SSLServer>(tls->certificate_path.c_str(), tls->private_key_path.c_str(), tls->client_ca_path.c_str());
                if (!secure->is_valid() || !secure->set_min_tls_version(TLS1_2_VERSION))
                    throw std::runtime_error("invalid remote object mTLS configuration");
                server_ = std::move(secure);
            }
            else
                server_ = std::make_unique<httplib::Server>();
            server_->Post("/v1/objects/put", [this](const httplib::Request &req,
                                                    httplib::Response &out)
                          {
            if(!authorized(req, token_)) return respond(out, 401, {{"error", "unauthorized"}});
            if(req.body.size() > kMaximumPayload)
                return respond(out, 413, {{"error", "request_too_large"}});
            try {
                const json value = json::parse(req.body);
                const auto tenant = value.at("tenant_id").get<std::string>();
                const auto bytes = value.at("bytes").get<std::string>();
                const auto media = value.at("media_type").get<std::string>();
                const auto expected = value.value("expected_digest", std::string{});
                std::string error;
                auto reference = store_.put(tenant, bytes, media, expected, &error);
                respond(out, reference ? 200 : 409,
                    {{"ok", reference.has_value()}, {"error", error},
                     {"reference", reference ? ref_json(*reference) : json(nullptr)}});
            } catch(const std::exception& error) {
                respond(out, 400, {{"error", error.what()}});
            } });
            server_->Post("/v1/objects/get", [this](const httplib::Request &req,
                                                    httplib::Response &out)
                          {
            if(!authorized(req, token_)) return respond(out, 401, {{"error", "unauthorized"}});
            if(req.body.size() > kMaximumPayload)
                return respond(out, 413, {{"error", "request_too_large"}});
            try {
                const json value = json::parse(req.body);
                const auto reference = parse_ref(value.at("reference"));
                std::string error;
                auto bytes = store_.get(reference, &error);
                respond(out, bytes ? 200 : 409,
                    {{"ok", bytes.has_value()}, {"error", error},
                     {"bytes", bytes ? json(*bytes) : json(nullptr)}});
            } catch(const std::exception& error) {
                respond(out, 400, {{"error", error.what()}});
            } });
        }
        ObjectStore &store_;
        std::string token_;
        bool tls_;
        std::unique_ptr<httplib::Server> server_;
    };
    RemoteObjectStoreServer::RemoteObjectStoreServer(ObjectStore &s, std::string t) : impl_(std::make_unique<Impl>(s, std::move(t), std::nullopt)) {}
    RemoteObjectStoreServer::RemoteObjectStoreServer(ObjectStore &s, std::string t, RemoteQueueTlsServerConfig tls) : impl_(std::make_unique<Impl>(s, std::move(t), std::move(tls))) {}
    RemoteObjectStoreServer::~RemoteObjectStoreServer() = default;
    int RemoteObjectStoreServer::bind(std::string_view host, int port)
    {
        if (!impl_->tls_ && host != "127.0.0.1" && host != "localhost" && host != "::1")
            return -1;
        const std::string address(host);
        return port == 0 ? impl_->server_->bind_to_any_port(address.c_str()) : (impl_->server_->bind_to_port(address.c_str(), port) ? port : -1);
    }
    bool RemoteObjectStoreServer::listen_after_bind() { return impl_->server_->listen_after_bind(); }
    void RemoteObjectStoreServer::stop() { impl_->server_->stop(); }
    RemoteObjectStoreClient::RemoteObjectStoreClient(std::string h, int p, std::string t, int timeout) : host_(std::move(h)), port_(p), bearer_token_(std::move(t)), timeout_seconds_(timeout)
    {
        if (host_.empty() || port_ <= 0 || port_ > 65535 || bearer_token_.empty() || timeout <= 0)
            throw std::invalid_argument("invalid remote object client configuration");
    }
    RemoteObjectStoreClient::RemoteObjectStoreClient(std::string h, int p, std::string t, RemoteQueueTlsClientConfig tls, int timeout) : host_(std::move(h)), port_(p), bearer_token_(std::move(t)), timeout_seconds_(timeout), tls_(std::move(tls))
    {
        if (host_.empty() || port_ <= 0 || port_ > 65535 || bearer_token_.empty() || timeout <= 0 || tls_->ca_path.empty() || tls_->certificate_path.empty() || tls_->private_key_path.empty())
            throw std::invalid_argument("invalid remote object mTLS client configuration");
    }
    std::optional<ObjectRef> RemoteObjectStoreClient::put(std::string_view tenant, std::string_view bytes, std::string_view media, std::string_view expected, std::string *error)
    {
        auto value = request(host_, port_, bearer_token_, timeout_seconds_, "/v1/objects/put", {{"tenant_id", tenant}, {"bytes", bytes}, {"media_type", media}, {"expected_digest", expected}}, tls_, error);
        return value ? std::optional<ObjectRef>(parse_ref(value->at("reference"))) : std::nullopt;
    }
    std::optional<std::string> RemoteObjectStoreClient::get(const ObjectRef &ref, std::string *error) const
    {
        auto value = request(host_, port_, bearer_token_, timeout_seconds_, "/v1/objects/get", {{"reference", ref_json(ref)}}, tls_, error);
        return value ? std::optional<std::string>(value->at("bytes").get<std::string>()) : std::nullopt;
    }
} // namespace agent_framework::distributed
