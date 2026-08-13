#pragma once

#include "agent/distributed/object_store.hpp"
#include "agent/distributed/remote_queue.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace agent_framework::distributed {

class RemoteObjectStoreServer {
public:
    RemoteObjectStoreServer(ObjectStore& store, std::string bearer_token);
    RemoteObjectStoreServer(ObjectStore& store, std::string bearer_token,
                            RemoteQueueTlsServerConfig tls);
    ~RemoteObjectStoreServer();
    RemoteObjectStoreServer(const RemoteObjectStoreServer&) = delete;
    RemoteObjectStoreServer& operator=(const RemoteObjectStoreServer&) = delete;
    int bind(std::string_view host, int port = 0);
    bool listen_after_bind();
    void stop();
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class RemoteObjectStoreClient final : public ObjectStore {
public:
    RemoteObjectStoreClient(std::string host, int port, std::string bearer_token,
                            int timeout_seconds = 2);
    RemoteObjectStoreClient(std::string host, int port, std::string bearer_token,
                            RemoteQueueTlsClientConfig tls, int timeout_seconds = 2);
    std::optional<ObjectRef> put(std::string_view tenant_id, std::string_view bytes,
                                 std::string_view media_type,
                                 std::string_view expected_digest = {},
                                 std::string* error = nullptr) override;
    std::optional<std::string> get(const ObjectRef& reference,
                                   std::string* error = nullptr) const override;
    ObjectStoreCapabilities capabilities() const noexcept override { return {true,true}; }
    ObjectListPage list(std::string_view tenant_id, std::string_view cursor = {}, std::size_t limit = 100) const override;
    ObjectRemoveResult remove(const ObjectRef&) override;
private:
    std::string host_;
    int port_;
    std::string bearer_token_;
    int timeout_seconds_;
    std::optional<RemoteQueueTlsClientConfig> tls_;
};

}  // namespace agent_framework::distributed
