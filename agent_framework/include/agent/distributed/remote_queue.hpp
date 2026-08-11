#pragma once

#include "agent/distributed/durable_queue.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace agent_framework::distributed {

struct RemoteQueueTlsServerConfig {
    std::string certificate_path;
    std::string private_key_path;
    std::string client_ca_path;
};

struct RemoteQueueTlsClientConfig {
    std::string ca_path;
    std::string certificate_path;
    std::string private_key_path;
};

// A deliberately narrow remote transport for the production scheduler path.
// The server always uses the atomic quota-aware queue operations; clients cannot
// accidentally lease work without reserving tenant capacity.
class RemoteQueueServer {
public:
    RemoteQueueServer(SQLiteDurableQueue& queue, std::string bearer_token);
    RemoteQueueServer(SQLiteDurableQueue& queue, std::string bearer_token,
                      RemoteQueueTlsServerConfig tls);
    ~RemoteQueueServer();
    RemoteQueueServer(const RemoteQueueServer&) = delete;
    RemoteQueueServer& operator=(const RemoteQueueServer&) = delete;

    int bind(std::string_view host, int port = 0);
    bool listen_after_bind();
    void stop();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class RemoteQueueClient {
public:
    RemoteQueueClient(std::string host, int port, std::string bearer_token,
                      int timeout_seconds = 2);
    RemoteQueueClient(std::string host, int port, std::string bearer_token,
                      RemoteQueueTlsClientConfig tls, int timeout_seconds = 2);

    bool enqueue(QueueTask task, std::string* error = nullptr) const;
    std::optional<Lease> claim(std::string_view worker_id, std::string_view tenant_id,
                               std::int64_t lease_ms,
                               std::string* error = nullptr) const;
    bool renew(std::string_view task_id, std::string_view worker_id,
               std::uint64_t fencing_token, std::int64_t lease_ms,
               std::string* error = nullptr) const;
    bool ack(std::string_view task_id, std::string_view worker_id,
             std::uint64_t fencing_token, std::string* error = nullptr) const;
    bool nack(std::string_view task_id, std::string_view worker_id,
              std::uint64_t fencing_token, std::int64_t retry_delay_ms,
              std::string* error = nullptr) const;
    std::optional<QueueTask> inspect(std::string_view task_id,
                                     std::string* error = nullptr) const;

private:
    std::string host_;
    int port_;
    std::string bearer_token_;
    int timeout_seconds_;
    std::optional<RemoteQueueTlsClientConfig> tls_;
};

}  // namespace agent_framework::distributed
